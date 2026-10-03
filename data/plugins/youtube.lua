-- YouTube (offizielle Kanaele, Suche, Playlisten)
--
-- Ansatz wie ViTube (https://github.com/shorelight82/vitube-vpk): YouTubes interne
-- "InnerTube"-JSON-API. Suche/Kanaele/Playlisten ueber den WEB-Client, der Stream ueber den
-- visionOS-Client, der ohne Anmeldung ein HLS-Manifest liefert - das spielt der Player mit
-- dem Hardware-Decoder bis 720p. Eigene Lua-Umsetzung, kein Code aus ViTube uebernommen.
--
-- Hinweis: YouTube aendert diese Schnittstelle gelegentlich. Wenn die Wiedergabe ploetzlich
-- scheitert, muessen meist nur die Client-Versionen unten angepasst werden.

local json = require("json")

local WEB_BASE = "https://www.youtube.com/youtubei/v1/"
local API_BASE = "https://youtubei.googleapis.com/youtubei/v1/"
local HL, GL = "de", "DE"

local WEB = { clientName = "WEB", clientVersion = "2.20260901.01.00", hl = HL, gl = GL,
              timeZone = "Europe/Berlin", utcOffsetMinutes = 120 }
local WEB_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36"

local VISION_UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15"
-- vollstaendiger Kontext (inkl. userAgent): ein knapper Kontext wird eher mit LOGIN_REQUIRED abgewiesen
local VISION = { clientName = "VISIONOS", clientVersion = "1.02", deviceMake = "Apple",
                 deviceModel = "RealityDevice17,1", userAgent = VISION_UA, osName = "visionOS",
                 osVersion = "26.5.23O471", hl = HL, gl = GL, timeZone = "UTC", utcOffsetMinutes = 0 }
-- Ersatz: Android-Client liefert direkte MP4-Dateien (Bild+Ton in einer Datei, meist 360p)
local ANDROID_UA = "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip"
local ANDROID = { clientName = "ANDROID", clientVersion = "21.26.364", androidSdkVersion = 30,
                  userAgent = ANDROID_UA, osName = "Android", osVersion = "11", hl = HL, gl = GL }
local CLIENT_IDS = { WEB = "1", ANDROID = "3", VISIONOS = "101" }

-- Kanal-Reiter (InnerTube-Parameter)
local TAB_VIDEOS    = "EgZ2aWRlb3PyBgQKAjoA"
local TAB_PLAYLISTS = "EglwbGF5bGlzdHPyBgQKAkIA"

-- Offizielle Kanaele: Anzeige, @Handle, Beschreibung. Laesst sich ein Handle nicht aufloesen,
-- wird stattdessen nach dem Namen gesucht.
local OFFICIAL = {
  { "Pokemon (offiziell)",      "@pokemon",            "Ganze Folgen Staffel 1-4, Filme zeitweise" },
  { "Pokemon Deutschland",      "@PokemonDeutschland", "Deutsche Clips & Folgen" },
  { "LEGO",                     "@LEGO",               "Serien, Kurzfilme" },
  { "Shaun das Schaf",          "@shaunthesheep",      "Folgen & Clips" },
  { "Cartoon Network",          "@cartoonnetwork",     "Clips und ganze Folgen (teils)" },
  { "Nickelodeon",              "@nickelodeon",        "Clips und Folgen" },
  { "Warner Bros. Animation",   "@WBAnimation",        "Looney Tunes u. a." },
  { "Disney Channel",           "@disneychannel",      "Clips & Kurzfolgen" },
  { "KiKA",                     "@KiKA",               "Kinderprogramm von ARD und ZDF" },
}
local ANIME = {
  { "TMS Anime (offiziell)",    "@TMSAnimeOfficial",   "Offizielle Anime von TMS" },
  { "Crunchyroll",              "@Crunchyroll",        "Offizielle Clips & erste Folgen" },
  { "Crunchyroll Deutschland",  "@CrunchyrollDE",      "Deutsche Clips & Folgen" },
  { "Muse Asia",                "@MuseAsia",           "Ganze Anime-Folgen (regional)" },
  { "Ani-One Asia",             "@AniOneAsia",         "Ganze Anime-Folgen (regional)" },
  { "AnimeLog",                 "@AnimeLog",           "Japanische Anime, teils mit Untertiteln" },
}

-- ---------------------------------------------------------------- Hilfen

local function enc(s) return (s:gsub("[^%w%-_%.~]", function(c) return string.format("%%%02X", c:byte()) end)) end
local function dec(s) return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end

-- X-Goog-Visitor-Id: jede Antwort enthaelt eine aktuelle; nur fuer diese Sitzung merken
-- (eine alte, gespeicherte Kennung fuehrt eher zu LOGIN_REQUIRED)
local visitor = nil

local function harvest_visitor(body)
  local v = body and body:match('"visitorData"%s*:%s*"([^"]+)"')
  if v then visitor = v end
end

local function call(endpoint, payload, client, ua, extra_hdr, base)
  payload.context = { client = client }
  local hdr = "Content-Type: application/json\nUser-Agent: " .. ua ..
              "\nX-Youtube-Client-Name: " .. (CLIENT_IDS[client.clientName] or "1") ..
              "\nX-Youtube-Client-Version: " .. client.clientVersion
  if not base then hdr = hdr .. "\nOrigin: https://www.youtube.com" end
  if visitor then hdr = hdr .. "\nX-Goog-Visitor-Id: " .. visitor end
  if extra_hdr then hdr = hdr .. "\n" .. extra_hdr end
  local body, status = vs.http_post((base or WEB_BASE) .. endpoint .. "?prettyPrint=false", json.encode(payload), hdr)
  if not body then return nil, "YouTube nicht erreichbar: " .. tostring(status) end
  harvest_visitor(body)
  local ok, data = pcall(json.decode, body)
  if not ok or type(data) ~= "table" then return nil, "YouTube-Antwort unlesbar (HTTP " .. tostring(status) .. ")" end
  if data.error then return nil, "YouTube: " .. tostring(data.error.message or data.error.status or "Fehler") end
  return data
end

local function web(endpoint, payload) return call(endpoint, payload, WEB, WEB_UA) end

local function ensure_visitor()
  if visitor then return end
  web("guide", {})   -- liefert responseContext.visitorData
end

-- Text aus {simpleText}, {runs=[...]}, {content=...} oder String
local function txt(x)
  if type(x) == "string" then return x end
  if type(x) ~= "table" then return nil end
  if type(x.simpleText) == "string" then return x.simpleText end
  if type(x.content) == "string" then return x.content end
  if type(x.runs) == "table" then
    local t = {}
    for _, r in ipairs(x.runs) do t[#t + 1] = r.text or "" end
    return table.concat(t)
  end
  return nil
end

-- erster Text im Teilbaum, der wie eine Dauer aussieht ("12:34", "1:02:03")
local function find_duration(node, depth)
  depth = depth or 0
  if depth > 12 or type(node) ~= "table" then return nil end
  for _, v in pairs(node) do
    if type(v) == "string" and v:match("^%d+:%d%d$") or (type(v) == "string" and v:match("^%d+:%d%d:%d%d$")) then return v end
    if type(v) == "table" then
      local d = find_duration(v, depth + 1)
      if d then return d end
    end
  end
  return nil
end

local function thumb_of(id) return "https://i.ytimg.com/vi/" .. id .. "/mqdefault.jpg" end

-- Durchsucht eine InnerTube-Antwort nach Videos, Kanaelen, Playlisten und Fortsetzung
local function collect(data)
  local videos, channels, playlists = {}, {}, {}
  local seen = {}
  local cont = nil
  local function walk(n, depth)
    if type(n) ~= "table" or depth > 60 then return end
    -- Videos (videoRenderer, compactVideoRenderer, gridVideoRenderer, playlistVideoRenderer, reelItemRenderer ...)
    if type(n.videoId) == "string" and (n.title or n.headline) and not seen[n.videoId] then
      local title = txt(n.title) or txt(n.headline)
      if title and #title > 0 then
        seen[n.videoId] = true
        videos[#videos + 1] = {
          id = n.videoId, title = title,
          channel = txt(n.ownerText) or txt(n.shortBylineText) or txt(n.longBylineText),
          duration = txt(n.lengthText) or find_duration(n.thumbnailOverlays),
          age = txt(n.publishedTimeText), index = txt(n.index),
        }
      end
    end
    -- neue "lockupViewModel"-Darstellung (Videos und Playlisten)
    if type(n.contentId) == "string" and n.contentType and not seen[n.contentId] then
      local meta = n.metadata and n.metadata.lockupMetadataViewModel
      local title = meta and txt(meta.title)
      if title then
        seen[n.contentId] = true
        if n.contentType == "LOCKUP_CONTENT_TYPE_VIDEO" then
          videos[#videos + 1] = { id = n.contentId, title = title, duration = find_duration(n.contentImage) }
        elseif n.contentType == "LOCKUP_CONTENT_TYPE_PLAYLIST" then
          playlists[#playlists + 1] = { id = n.contentId, title = title }
        end
      end
    end
    -- klassische Playlist-Kacheln
    if type(n.playlistId) == "string" and n.title and not n.videoId and not seen["PL" .. n.playlistId] then
      local title = txt(n.title)
      if title then
        seen["PL" .. n.playlistId] = true
        playlists[#playlists + 1] = { id = n.playlistId, title = title,
                                      count = txt(n.videoCountText) or txt(n.videoCountShortText) }
      end
    end
    -- Kanaele in Suchergebnissen
    if type(n.channelId) == "string" and n.title and not n.videoId and not seen["CH" .. n.channelId] then
      local title = txt(n.title)
      if title then
        seen["CH" .. n.channelId] = true
        channels[#channels + 1] = { id = n.channelId, title = title,
                                    sub = txt(n.videoCountText) or txt(n.subscriberCountText) }
      end
    end
    -- Fortsetzung (naechste Seite)
    if n.continuationCommand and type(n.continuationCommand.token) == "string" then cont = n.continuationCommand.token end
    if n.nextContinuationData and type(n.nextContinuationData.continuation) == "string" then
      cont = n.nextContinuationData.continuation
    end
    for _, v in pairs(n) do
      if type(v) == "table" then walk(v, depth + 1) end
    end
  end
  walk(data, 0)
  return videos, channels, playlists, cont
end

local function video_item(v, number)
  local sub = {}
  if v.channel then sub[#sub + 1] = v.channel end
  if v.duration then sub[#sub + 1] = v.duration end
  if v.age then sub[#sub + 1] = v.age end
  local title = v.title
  if number and v.index and v.index:match("^%d+$") then title = v.index .. ". " .. title end
  return { title = title, subtitle = table.concat(sub, "  |  "), id = "v:" .. v.id, kind = "video",
           thumb = thumb_of(v.id) }
end

local function list_from(data, more_kind, extra)
  local videos, channels, playlists, cont = collect(data)
  local items = {}
  for _, c in ipairs((extra and extra.no_channels) and {} or channels) do
    items[#items + 1] = { title = c.title, subtitle = "Kanal" .. (c.sub and ("  |  " .. c.sub) or ""),
                          id = "chid:" .. c.id, kind = "folder" }
  end
  for _, p in ipairs(playlists) do
    items[#items + 1] = { title = p.title, subtitle = "Playlist" .. (p.count and ("  |  " .. p.count) or ""),
                          id = "pl:" .. p.id, kind = "folder" }
  end
  for _, v in ipairs(videos) do items[#items + 1] = video_item(v, extra and extra.numbered) end
  if cont and #items > 0 then
    items[#items + 1] = { title = "Weitere laden", id = more_kind .. ":" .. cont, kind = "more" }
  end
  return items
end

-- @Handle -> Kanal-ID (UC...)
local handle_cache = {}
local function resolve_handle(handle)
  if handle_cache[handle] then return handle_cache[handle] end
  local data = web("navigation/resolve_url", { url = "https://www.youtube.com/" .. handle })
  local id = data and data.endpoint and data.endpoint.browseEndpoint and data.endpoint.browseEndpoint.browseId
  if id then handle_cache[handle] = id end
  return id
end

local function channel_home(chid, name)
  return {
    { title = "Videos", subtitle = name or "", id = "chv:" .. chid, kind = "folder" },
    { title = "Playlisten (Staffeln, Reihen)", subtitle = "ganze Folgen meist hier, in Reihenfolge",
      id = "chp:" .. chid, kind = "folder" },
  }
end

local function channel_list(list)
  local items = {}
  for _, c in ipairs(list) do
    items[#items + 1] = { title = c[1], subtitle = c[3], id = "h:" .. c[2] .. "|" .. enc(c[1]), kind = "folder" }
  end
  return items
end

local function do_search(q)
  ensure_visitor()
  local data, err = web("search", { query = q })
  if not data then return nil, err end
  local items = list_from(data, "sm")
  if #items == 0 then return nil, "Nichts gefunden fuer: " .. q end
  return items
end

-- ---------------------------------------------------------------- Quelle

return {
  name = "YouTube",
  save_ref = true,   -- Stream-Adressen laufen ab: in Playlists Verweis speichern
  description = "Offizielle Kanaele (Pokemon, LEGO, Anime ...), Suche, Playlisten - bis 720p",

  browse = function(id)
    if id == nil then
      return {
        { title = "Suchen ...", subtitle = "Videos, Kanaele, Playlisten", id = "search", kind = "search" },
        { title = "Offizielle Kanaele", subtitle = "Pokemon, LEGO, Shaun das Schaf, Cartoon Network ...",
          id = "official", kind = "folder" },
        { title = "Anime (offiziell)", subtitle = "TMS, Crunchyroll, Muse Asia, Ani-One ...", id = "anime", kind = "folder" },
      }
    end
    if id == "official" then return channel_list(OFFICIAL) end
    if id == "anime" then return channel_list(ANIME) end

    ensure_visitor()
    local handle, name = id:match("^h:([^|]+)|(.*)$")
    if handle then
      name = dec(name)
      local chid = resolve_handle(handle)
      if chid then return channel_home(chid, name) end
      vs.log("Kanal " .. handle .. " nicht gefunden - suche nach Namen")
      return do_search(name)
    end
    local chid = id:match("^chid:(.+)$")
    if chid then return channel_home(chid) end

    local cv = id:match("^chv:(.+)$")
    if cv then
      local data, err = web("browse", { browseId = cv, params = TAB_VIDEOS })
      if not data then return nil, err end
      local items = list_from(data, "bm", { no_channels = true })
      if #items == 0 then return nil, "Keine Videos gefunden" end
      return items
    end
    local cp = id:match("^chp:(.+)$")
    if cp then
      local data, err = web("browse", { browseId = cp, params = TAB_PLAYLISTS })
      if not data then return nil, err end
      local items = list_from(data, "bm", { no_channels = true })
      if #items == 0 then return nil, "Keine Playlisten gefunden" end
      return items
    end
    local pl = id:match("^pl:(.+)$")
    if pl then
      local data, err = web("browse", { browseId = pl:match("^VL") and pl or ("VL" .. pl) })
      if not data then return nil, err end
      local items = list_from(data, "bmn", { numbered = true, no_channels = true })
      if #items == 0 then return nil, "Playlist ist leer oder privat" end
      return items
    end

    -- Fortsetzungen
    local kind, token = id:match("^(%a+):(.+)$")
    if kind == "sm" then
      local data, err = web("search", { continuation = token })
      if not data then return nil, err end
      return list_from(data, "sm")
    elseif kind == "bm" or kind == "bmn" then
      local data, err = web("browse", { continuation = token })
      if not data then return nil, err end
      return list_from(data, kind, { numbered = kind == "bmn", no_channels = true })
    end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(query)
    local q = (query or ""):gsub("^%s+", ""):gsub("%s+$", "")
    if q == "" then return nil, "Bitte einen Suchbegriff eingeben" end
    return do_search(q)
  end,

  resolve = function(item)
    local vid = item.id:match("^v:(.+)$") or item.id
    ensure_visitor()
    local reasons = {}
    local function why(ps)
      local r = ps and (ps.reason or (ps.errorScreen and ps.errorScreen.playerErrorMessageRenderer and
                txt(ps.errorScreen.playerErrorMessageRenderer.reason)) or ps.status)
      if r and not reasons[r] then reasons[r] = true; reasons[#reasons + 1] = r end
    end
    local body = function()
      return { videoId = vid, contentCheckOk = true, racyCheckOk = true,
               playbackContext = { contentPlaybackContext = { html5Preference = "HTML5_PREF_WANTS" } } }
    end

    -- 1. visionOS: HLS bis 720p (auch Livestreams). Die erste Abweisung liefert eine frische
    --    Besucher-ID mit - damit genau einmal wiederholen.
    for attempt = 1, 2 do
      local before = visitor
      local data, err = call("player", body(), VISION, VISION_UA)
      if not data then reasons[#reasons + 1] = err; break end
      local sd = data.streamingData or {}
      if sd.hlsManifestUrl then
        return { url = sd.hlsManifestUrl, headers = { ["User-Agent"] = VISION_UA } }
      end
      why(data.playabilityStatus)
      local st = data.playabilityStatus and data.playabilityStatus.status
      if not (st == "LOGIN_REQUIRED" and visitor ~= before and attempt == 1) then break end
    end

    -- 2. Android: direkte MP4-Datei (Bild und Ton zusammen)
    local data = call("player", { videoId = vid, contentCheckOk = true, racyCheckOk = true },
                      ANDROID, ANDROID_UA, nil, API_BASE)
    if data then
      local best
      for _, f in ipairs((data.streamingData or {}).formats or {}) do
        local mime = f.mimeType or ""
        if f.url and mime:find("avc1", 1, true) and (tonumber(f.height) or 0) <= 720 then
          if not best or (tonumber(f.height) or 0) > (tonumber(best.height) or 0) then best = f end
        end
      end
      if best then return { url = best.url, headers = { ["User-Agent"] = ANDROID_UA } } end
      why(data.playabilityStatus)
    end

    local msg = #reasons > 0 and table.concat(reasons, " / ") or "kein abspielbarer Stream"
    if msg:find("LOGIN_REQUIRED") or msg:lower():find("bot") or msg:find("anmelden") or msg:find("Sign in") then
      msg = msg .. "  -  YouTube verlangt hier eine Anmeldung (Altersfreigabe oder Bot-Pruefung). " ..
            "Oft hilft es, kurz zu warten und es erneut zu versuchen."
    end
    return nil, "YouTube: " .. msg
  end,
}
