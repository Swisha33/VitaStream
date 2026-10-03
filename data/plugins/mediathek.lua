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
local function query(queries, offset, size, min_minutes, order)
  local body = json.encode({
    queries = queries,
    sortBy = "timestamp", sortOrder = order or "desc", future = false,
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
-- Staffel/Folge aus dem Titel: "(S02/E05)", "Staffel 2 Folge 5", "Folge 12", "Teil 3"
local function episode_key(title)
  local s, e = title:match("[Ss](%d+)%s*/%s*[Ee](%d+)")
  if not s then s, e = title:match("[Ss]taffel%s*(%d+).-[Ff]olge%s*(%d+)") end
  if not s then s, e = title:match("[Ss](%d+)[Ee](%d+)") end
  if s then return tonumber(s) * 10000 + tonumber(e) end
  local n = title:match("[Ff]olge%s*(%d+)") or title:match("[Tt]eil%s*(%d+)") or title:match("[Ee]pisode%s*(%d+)")
  return n and tonumber(n) or nil
end

local function results_page(queries, offset, min_minutes, more_prefix, show_channel, exact_topic)
  -- Sendereihen in Erscheinungsreihenfolge (aelteste zuerst), sonst neueste zuerst
  local res, total = query(queries, offset, PAGE, min_minutes, exact_topic and "asc" or "desc")
  if not res then return nil, total end
  local items = {}
  for _, r in ipairs(res) do
    -- Themen-Suche ist eine Teilwort-Suche: nur genau diese Sendereihe anzeigen
    if not exact_topic or r.topic == exact_topic then
      local it = to_item(r, show_channel)
      if it then
        if exact_topic then it.title = r.title end   -- Reihenname steht schon in der Kopfzeile
        items[#items + 1] = it
      end
    end
  end
  -- innerhalb der Seite nach Staffel/Folge ordnen, wenn alle Titel eine Nummer haben
  if exact_topic and #items > 1 then
    local all = true
    for i, it in ipairs(items) do
      it._k = episode_key(it.title)
      it._pos = i
      if not it._k then all = false end
    end
    if all then table.sort(items, function(a, b) if a._k ~= b._k then return a._k < b._k end return a._pos < b._pos end) end
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

-- Wie results_page, aber Folgen derselben Sendereihe (Thema + Sender) in einem Ordner
local GROUP_PAGE = 200
local function grouped_page(queries, offset, min_minutes, more_prefix, show_channel)
  local res, total = query(queries, offset, GROUP_PAGE, min_minutes)
  if not res then return nil, total end
  local order, groups = {}, {}
  for _, r in ipairs(res) do
    local key = (r.channel or "") .. "\1" .. (r.topic or "")
    if not groups[key] then groups[key] = {}; order[#order + 1] = key end
    table.insert(groups[key], r)
  end
  local items = {}
  for _, key in ipairs(order) do
    local g = groups[key]
    local r = g[1]
    if #g == 1 or not r.topic or #r.topic == 0 then
      for _, x in ipairs(g) do
        local it = to_item(x, show_channel)
        if it then items[#items + 1] = it end
      end
    else
      items[#items + 1] = {
        title = r.topic, kind = "folder",
        subtitle = string.format("%d Folgen  |  %s  |  neueste %s", #g, r.channel or "?", fmt_date(r.timestamp)),
        id = "topic:" .. enc(r.channel or "") .. ":" .. enc(r.topic) .. ":0",
        thumb = (r.url_website and #r.url_website > 0) and ("og:" .. r.url_website) or nil,
      }
    end
  end
  local shown = offset + #res
  if #res == GROUP_PAGE and (not total or shown < total) then
    items[#items + 1] = {
      title = total and string.format("Weitere Sendungen laden (%d von %d Beitraegen)", shown, total) or "Weitere laden",
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
      local items = {
        { title = "Suchen ...", subtitle = "Sendung, Thema oder Titel in allen Mediatheken", kind = "search" },
        folder("Alle Sender", "ch:" .. enc(""), "Kategorien ueber alle Mediatheken"),
      }
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
      return grouped_page(ch_query(dec(nch)), tonumber(noff), 2, "new:" .. nch .. ":", dec(nch) == "")
    end

    local cch, ci, coff = id:match("^cat:(.-):(%d+):(%d+)$")
    if cch then
      local c = CATEGORIES[tonumber(ci)]
      local q = with(ch_query(dec(cch)), { fields = { "topic", "title", "description" }, query = c[2] })
      return grouped_page(q, tonumber(coff), c[3], "cat:" .. cch .. ":" .. ci .. ":", dec(cch) == "")
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
      local topic = dec(otopic)
      local q = with(ch_query(dec(och)), { fields = { "topic" }, query = topic })
      local items, err = results_page(q, tonumber(ooff), 1, "topic:" .. och .. ":" .. otopic .. ":", false, topic)
      return items, err
    end

    local sq, soff = id:match("^search:(.-):(%d+)$")
    if sq then
      return grouped_page({ { fields = { "title", "topic" }, query = dec(sq) } }, tonumber(soff), 2,
                          "search:" .. sq .. ":", true)
    end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(text)
    local items, err = grouped_page({ { fields = { "title", "topic" }, query = text } }, 0, 2,
                                    "search:" .. enc(text) .. ":", true)
    if not items then return nil, err == "Keine Sendungen gefunden" and ("Nichts gefunden fuer: " .. text) or err end
    return items
  end,

  resolve = function(item)
    return item.id
  end,
}
