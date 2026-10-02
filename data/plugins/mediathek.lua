-- Oeffentlich-rechtliche Mediatheken (ARD, ZDF, arte, 3sat, ...) ueber die
-- offene MediathekViewWeb-API. Liefert direkte MP4-/HLS-Links.

local json = require("json")
local API = "https://mediathekviewweb.de/api/query"

-- Qualitaet: "low" (schont WLAN/Akku), "normal" (meist 960x540 - passt zur Vita), "hd"
local QUALITY = "normal"

local CHANNELS = { "ARD", "ZDF", "arte.de", "3Sat", "PHOENIX", "ZDFneo", "ZDFinfo",
                   "BR", "NDR", "WDR", "SWR", "MDR", "HR", "RBB", "KiKA", "ORF", "SRF" }

local function fmt_duration(sec)
  sec = tonumber(sec) or 0
  if sec >= 3600 then return string.format("%d:%02d:%02d", sec // 3600, (sec // 60) % 60, sec % 60) end
  return string.format("%d:%02d", sec // 60, sec % 60)
end

local function fmt_date(ts)
  ts = tonumber(ts)
  if not ts then return "" end
  -- einfache Umrechnung Unix-Zeit -> Datum (UTC), ohne os.date
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

local function query(fields, text, size)
  local body = json.encode({
    queries = { { fields = fields, query = text } },
    sortBy = "timestamp",
    sortOrder = "desc",
    future = false,
    offset = 0,
    size = size or 60,
    duration_min = 120,
  })
  local res, status = vs.http_post(API, body, "Content-Type: text/plain")
  if not res then return nil, status end
  local data, err = json.decode(res)
  if not data then return nil, "Antwort unlesbar: " .. tostring(err) end
  if data.err then return nil, "API-Fehler: " .. json.encode(data.err) end
  return data.result and data.result.results or {}
end

local function to_items(results)
  local items = {}
  for _, r in ipairs(results) do
    local url = r.url_video
    if QUALITY == "low" and r.url_video_low and #r.url_video_low > 0 then url = r.url_video_low end
    if QUALITY == "hd" and r.url_video_hd and #r.url_video_hd > 0 then url = r.url_video_hd end
    if url and #url > 0 then
      items[#items + 1] = {
        title = (r.topic and r.topic ~= r.title) and (r.topic .. " - " .. r.title) or r.title,
        subtitle = string.format("%s  |  %s  |  %s", r.channel or "?", fmt_date(r.timestamp),
                                 fmt_duration(r.duration)),
        id = url,
        kind = "video",
      }
    end
  end
  return items
end

return {
  name = "Mediatheken (ARD, ZDF, arte ...)",
  description = "Oeffentlich-rechtliche Sendungen - Dreieck zum Suchen",

  browse = function(id)
    if id == nil then
      local items = {}
      for _, ch in ipairs(CHANNELS) do
        items[#items + 1] = { title = ch, subtitle = "Neueste Sendungen", id = "ch:" .. ch, kind = "folder" }
      end
      return items
    end
    local ch = id:match("^ch:(.+)$")
    if ch then
      local res, err = query({ "channel" }, ch, 80)
      if not res then return nil, err end
      return to_items(res)
    end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(text)
    local res, err = query({ "title", "topic" }, text, 80)
    if not res then return nil, err end
    local items = to_items(res)
    if #items == 0 then return nil, "Nichts gefunden fuer: " .. text end
    return items
  end,

  resolve = function(item)
    -- manche Sender liefern http-Links; https funktioniert meist ebenfalls
    return item.id
  end,
}
