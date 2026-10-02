-- Oeffentlich-rechtliche Mediatheken (ARD, ZDF, arte, 3sat, ...) ueber die offene
-- MediathekViewWeb-API. Pro Sender: Neueste, Sendungen, Kategorien; alles seitenweise
-- ("Weitere laden"), Vorschaubilder kommen von der jeweiligen Sendungsseite.

local json = require("json")
local API = "https://mediathekviewweb.de/api/query"
local PAGE = 50

-- Qualitaet: "low", "normal" (meist 960x540 - passt zur Vita), "hd" (720p/1080p - 1080p geht nicht)
local QUALITY = "normal"

local CHANNELS = { "ARD", "ZDF", "arte.de", "3Sat", "PHOENIX", "ZDFneo", "ZDFinfo", "ZDFtivi", "KiKA",
                   "BR", "NDR", "WDR", "SWR", "MDR", "HR", "RBB", "SR", "radiobremen", "ONE", "ARD-alpha",
                   "tagesschau24", "Funk.net", "DW", "ORF", "SRF" }

-- Kategorien: Suchbegriff (Thema/Titel/Beschreibung) und Mindestlaenge in Minuten
local CATEGORIES = {
  { "Filme",              "film",        70 },
  { "Dokumentationen",    "doku",        15 },
  { "Krimis",             "krimi",       40 },
  { "Tatort & Polizeiruf","tatort",      60 },
  { "Serien",             "folge",       15 },
  { "Comedy & Satire",    "satire",       5 },
  { "Kinder",             "kinder",       5 },
  { "Anime & Zeichentrick","anime",       5 },
  { "Nachrichten",        "nachrichten",  5 },
  { "Wissen",             "wissen",      10 },
  { "Musik & Konzerte",   "konzert",     20 },
  { "Sport",              "sport",        5 },
  { "Reisen",             "reise",       15 },
  { "Kochen",             "koch",        10 },
}

local function enc(s) return (s:gsub("[^%w%-_%.~ ]", function(c) return string.format("%%%02X", c:byte()) end)) end
local function dec(s) return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end

local function fmt_duration(sec)
  sec = tonumber(sec) or 0
  if sec >= 3600 then return string.format("%d:%02d:%02d", sec // 3600, (sec // 60) % 60, sec % 60) end
  return string.format("%d:%02d", sec // 60, sec % 60)
end

local function fmt_date(ts)
  ts = tonumber(ts)
  if not ts then return "" end
  local days = ts // 86400
  local z = days + 719468
  local era = (z >= 0 and z or z - 146096) // 146097
  local doe = z - era * 146097
  local yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
  local y = yoe + era * 400
  local doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
  local mp = (5 * doy + 2) // 153
  local d = doy - (153 * mp + 2) // 5 + 1
  local m = mp < 10 and mp + 3 or mp - 9
  if m <= 2 then y = y + 1 end
  return string.format("%02d.%02d.%d", d, m, y)
end

-- queries: Liste von { fields = {...}, query = "..." } (UND-verknuepft)
local function query(queries, offset, size, min_minutes)
  local body = json.encode({
    queries = queries,
    sortBy = "timestamp", sortOrder = "desc", future = false,
    offset = offset or 0, size = size or PAGE,
    duration_min = (min_minutes or 2) * 60,
  })
  local res, status = vs.http_post(API, body, "Content-Type: text/plain")
  if not res then return nil, status end
  local data, err = json.decode(res)
  if not data then return nil, "Antwort unlesbar: " .. tostring(err) end
  if data.err then return nil, "API-Fehler: " .. json.encode(data.err) end
  local r = data.result or {}
  local total = r.queryInfo and r.queryInfo.totalResults or #(r.results or {})
  return r.results or {}, total
end

local function to_item(r, show_channel)
  local url = r.url_video
  if QUALITY == "low" and r.url_video_low and #r.url_video_low > 0 then url = r.url_video_low end
  if QUALITY == "hd" and r.url_video_hd and #r.url_video_hd > 0 then url = r.url_video_hd end
  if not url or #url == 0 then return nil end
  local title = (r.topic and r.title and not r.title:find(r.topic, 1, true)) and (r.topic .. " - " .. r.title) or r.title
  local sub = string.format("%s%s  |  %s", show_channel and ((r.channel or "?") .. "  |  ") or "",
                            fmt_date(r.timestamp), fmt_duration(r.duration))
  return {
    title = title, subtitle = sub, id = url, kind = "video",
    thumb = (r.url_website and #r.url_website > 0) and ("og:" .. r.url_website) or nil,
  }
end

-- Ergebnisliste + "Weitere laden"
local function results_page(queries, offset, min_minutes, more_prefix, show_channel)
  local res, total = query(queries, offset, PAGE, min_minutes)
  if not res then return nil, total end
  local items = {}
  for _, r in ipairs(res) do
    local it = to_item(r, show_channel)
    if it then items[#items + 1] = it end
  end
  local shown = offset + #res
  if #res == PAGE and (not total or shown < total) then
    items[#items + 1] = {
      title = total and string.format("Weitere laden (%d von %d)", shown, total) or "Weitere laden",
      kind = "more", id = more_prefix .. shown,
    }
  end
  if #items == 0 then return nil, "Keine Sendungen gefunden" end
  return items
end

local function ch_query(ch)
  if ch == "" then return {} end
  return { { fields = { "channel" }, query = ch } }
end

local function with(base, extra)
  local q = {}
  for _, v in ipairs(base) do q[#q + 1] = v end
  q[#q + 1] = extra
  return q
end

local function folder(title, id, sub) return { title = title, id = id, kind = "folder", subtitle = sub } end

return {
  name = "Mediatheken (ARD, ZDF, arte ...)",
  description = "Oeffentlich-rechtliche Sendungen nach Sender und Kategorie - Dreieck zum Suchen",

  browse = function(id)
    if id == nil then
      local items = { folder("Alle Sender", "ch:" .. enc(""), "Kategorien ueber alle Mediatheken") }
      for _, ch in ipairs(CHANNELS) do items[#items + 1] = folder(ch, "ch:" .. enc(ch)) end
      return items
    end

    -- Sender-Startseite: Neueste, Sendungen, Kategorien
    local ch = id:match("^ch:(.*)$")
    if ch then
      local e = ch
      local items = {
        folder("Neueste", "new:" .. e .. ":0", "Zuletzt veroeffentlicht"),
        folder("Sendungen", "topics:" .. e, "Nach Sendereihe sortiert"),
      }
      for ci, c in ipairs(CATEGORIES) do items[#items + 1] = folder(c[1], "cat:" .. e .. ":" .. ci .. ":0") end
      return items
    end

    local nch, noff = id:match("^new:(.-):(%d+)$")
    if nch then
      return results_page(ch_query(dec(nch)), tonumber(noff), 2, "new:" .. nch .. ":", dec(nch) == "")
    end

    local cch, ci, coff = id:match("^cat:(.-):(%d+):(%d+)$")
    if cch then
      local c = CATEGORIES[tonumber(ci)]
      local q = with(ch_query(dec(cch)), { fields = { "topic", "title", "description" }, query = c[2] })
      return results_page(q, tonumber(coff), c[3], "cat:" .. cch .. ":" .. ci .. ":", dec(cch) == "")
    end

    -- Sendereihen aus den neuesten Beitraegen
    local tch = id:match("^topics:(.*)$")
    if tch then
      vs.log("Sammle Sendungen ...")
      local res, err = query(ch_query(dec(tch)), 0, 600, 2)
      if not res then return nil, err end
      local count, order = {}, {}
      for _, r in ipairs(res) do
        local t = r.topic or ""
        if #t > 0 then
          if not count[t] then count[t] = 0; order[#order + 1] = t end
          count[t] = count[t] + 1
        end
      end
      table.sort(order, function(a, b) return a:lower() < b:lower() end)
      local items = {}
      for _, t in ipairs(order) do
        items[#items + 1] = folder(t, "topic:" .. tch .. ":" .. enc(t) .. ":0", count[t] .. " neue Beitraege")
      end
      if #items == 0 then return nil, "Keine Sendungen gefunden" end
      return items
    end

    local och, otopic, ooff = id:match("^topic:(.-):(.-):(%d+)$")
    if och then
      local q = with(ch_query(dec(och)), { fields = { "topic" }, query = dec(otopic) })
      return results_page(q, tonumber(ooff), 1, "topic:" .. och .. ":" .. otopic .. ":", false)
    end

    local sq, soff = id:match("^search:(.-):(%d+)$")
    if sq then
      return results_page({ { fields = { "title", "topic" }, query = dec(sq) } }, tonumber(soff), 2,
                          "search:" .. sq .. ":", true)
    end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(text)
    local items, err = results_page({ { fields = { "title", "topic" }, query = text } }, 0, 2,
                                    "search:" .. enc(text) .. ":", true)
    if not items then return nil, err == "Keine Sendungen gefunden" and ("Nichts gefunden fuer: " .. text) or err end
    return items
  end,

  resolve = function(item)
    return item.id
  end,
}
