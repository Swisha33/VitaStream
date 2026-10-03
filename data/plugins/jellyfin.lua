-- Jellyfin-Client: meldet sich am eigenen Jellyfin-Server an und zeigt die Mediathek
-- so an, wie sie dort aufgebaut ist (Ansichten -> Ordner/Sammlungen -> Serien -> Staffeln -> Folgen).
--
-- Anmeldung in drei Schritten: Server-Adresse, Benutzername, Passwort.
-- Das Passwort wird nie gespeichert - nur das Zugriffs-Token des Servers, und das
-- verschluesselt mit einem Schluessel, der an diese Vita gebunden ist (vs.secret_*).
-- Abmelden loescht das Token und meldet die Sitzung auch am Server ab.
--
-- Wiedergabe wie bei den offiziellen Clients (und Switchfin): die App schickt dem Server ein
-- Geraeteprofil (was die Vita kann). Der Server entscheidet: direkt abspielen oder in
-- H.264/AAC bis 720p umwandeln. So laufen auch MKV, HEVC, 10-Bit, DTS usw.

local json = require("json")

local SECRET = "jellyfin"            -- verschluesselte Sitzung (Server, Token, Benutzer-ID)
local LAST   = "jellyfin_last.txt"   -- nur Server-Adresse und Benutzername als Vorschlag
local OLD_SESS = "jellyfin_session.txt"
local CLIENT = "VitaStream"
local DEVICE = "PSVita"
local VERSION = "0.8"
local PAGE = 100            -- Eintraege pro Seite
local MAXH, MAXW = 720, 1280

-- ---------------------------------------------------------------- Hilfen

local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

local function parse_kv(txt)
  local t = {}
  for line in (txt or ""):gmatch("[^\r\n]+") do
    local k, v = line:match("^([%w_]+)=(.*)$")
    if k then t[k] = v end
  end
  return t
end

local function read_session()
  -- alte, unverschluesselte Sitzung aus 0.6/0.7 einmalig umziehen und loeschen
  local old = vs.read_file(OLD_SESS)
  if old and #old > 0 then
    local o = parse_kv(old)
    if o.server and o.token and o.uid then
      vs.secret_set(SECRET, string.format("server=%s\ntoken=%s\nuid=%s", o.server, o.token, o.uid))
    end
    vs.write_file(OLD_SESS, "")
    vs.delete_file(OLD_SESS)
  end
  local plain = vs.read_file("jellyfin.txt")
  if plain and plain:find("pass") then
    vs.write_file("jellyfin.txt", "")      -- Klartext-Passwort aus aelteren Versionen entfernen
    vs.delete_file("jellyfin.txt")
  end
  local t = parse_kv(vs.secret_get(SECRET))
  if t.server and t.token and t.uid then return { server = t.server, token = t.token, uid = t.uid } end
  return nil
end

local function last_login()
  return parse_kv(vs.read_file(LAST))
end

local function auth_header(token)
  local h = string.format('MediaBrowser Client="%s", Device="%s", DeviceId="%s-%s", Version="%s"',
                          CLIENT, DEVICE, DEVICE, CLIENT, VERSION)
  if token and token ~= "" then h = h .. ', Token="' .. token .. '"' end
  return "Authorization: " .. h
end

local function api_get(sess, path)
  local sep = path:find("?", 1, true) and "&" or "?"
  local url = sess.server .. path .. sep .. "api_key=" .. sess.token
  local body, status = vs.http_get(url, auth_header(sess.token) .. "\nAccept: application/json")
  if not body then return nil, status end
  if status == 401 then return nil, "Nicht angemeldet (Token abgelaufen) - bitte neu anmelden" end
  if status and status >= 400 then return nil, "Server-Fehler HTTP " .. status end
  local ok, data = pcall(json.decode, body)
  if not ok then return nil, "Ungueltige Antwort vom Server" end
  return data
end

-- ---------------------------------------------------------------- Anmeldung

local function login(server, user, pass)
  server = server:gsub("/+$", "")
  if not server:match("^https?://") then server = "http://" .. server end
  local body = json.encode({ Username = user, Pw = pass or "" })
  vs.log("Anmeldung an " .. server)
  local resp, status = vs.http_post(server .. "/Users/AuthenticateByName", body,
    auth_header(nil) .. "\nContent-Type: application/json\nAccept: application/json")
  if not resp then return nil, "Server nicht erreichbar: " .. tostring(status) end
  if status == 401 then return nil, "Benutzername oder Passwort falsch" end
  if status and status >= 400 then return nil, "Anmeldung fehlgeschlagen (HTTP " .. status .. ")" end
  local ok, data = pcall(json.decode, resp)
  if not ok or type(data) ~= "table" or not data.AccessToken then
    return nil, "Anmeldung fehlgeschlagen: unerwartete Antwort"
  end
  local sess = { server = server, token = data.AccessToken, uid = data.User and data.User.Id }
  if not sess.uid then return nil, "Anmeldung fehlgeschlagen: keine Benutzer-ID" end
  if not vs.secret_set(SECRET, string.format("server=%s\ntoken=%s\nuid=%s", sess.server, sess.token, sess.uid)) then
    vs.log("Token konnte nicht gesichert werden - Anmeldung gilt nur bis zum Beenden")
  end
  vs.write_file(LAST, string.format("server=%s\nuser=%s\n", sess.server, user))
  return sess
end

-- gueltige Sitzung holen (einmal pro App-Start beim Server pruefen)
local mem_session, checked = nil, false
local function ensure_session()
  if mem_session and checked then return mem_session end
  local sess = mem_session or read_session()
  if not sess then return nil, "setup" end
  local test, err = api_get(sess, "/System/Info")
  if not test then
    if tostring(err):find("Nicht angemeldet") then vs.secret_set(SECRET, nil); mem_session = nil; return nil, "setup" end
    return nil, "Server nicht erreichbar: " .. tostring(err)
  end
  mem_session, checked = sess, true
  return sess
end

-- ---------------------------------------------------------------- Mediathek abbilden

local function image_url(sess, item)
  local tags = item.ImageTags
  if tags and tags.Primary then
    return string.format("%s/Items/%s/Images/Primary?maxHeight=180&tag=%s&api_key=%s",
                         sess.server, item.Id, tags.Primary, sess.token)
  end
  if item.SeriesId and item.SeriesPrimaryImageTag then
    return string.format("%s/Items/%s/Images/Primary?maxHeight=180&tag=%s&api_key=%s",
                         sess.server, item.SeriesId, item.SeriesPrimaryImageTag, sess.token)
  end
  return nil
end

local function ticks_to_text(t)
  if not t or t <= 0 then return nil end
  local s = math.floor(t / 10000000)
  if s >= 3600 then return string.format("%d:%02d:%02d", s // 3600, (s // 60) % 60, s % 60) end
  return string.format("%d:%02d", s // 60, s % 60)
end

-- Typen, die man abspielen kann
local PLAYABLE = { Movie = 1, Episode = 1, Video = 1, MusicVideo = 1, Audio = 1, TvChannel = 1, Trailer = 1 }
-- Typen, die man oeffnet (Ordner)
local FOLDERISH = { CollectionFolder = 1, Folder = 1, UserView = 1, BoxSet = 1, Series = 1, Season = 1,
                    Playlist = 1, MusicAlbum = 1, MusicArtist = 1, Genre = 1, PhotoAlbum = 1, Channel = 1 }

local function item_to_entry(sess, it)
  local playable = PLAYABLE[it.Type] and not it.IsFolder
  local sub
  if it.Type == "Series" then
    sub = (it.ProductionYear and (it.ProductionYear .. "  ") or "") ..
          (it.ChildCount and (it.ChildCount .. " Staffeln") or "Serie")
  elseif it.Type == "Season" then
    sub = (it.ChildCount and (it.ChildCount .. " Folgen")) or "Staffel"
  elseif it.Type == "Episode" then
    local e = it.IndexNumber and ("Folge " .. it.IndexNumber) or "Folge"
    if it.ParentIndexNumber then e = "S" .. it.ParentIndexNumber .. " " .. e end
    sub = e .. (ticks_to_text(it.RunTimeTicks) and ("  |  " .. ticks_to_text(it.RunTimeTicks)) or "")
  elseif it.Type == "Movie" then
    sub = (it.ProductionYear and (it.ProductionYear .. "") or "Film") ..
          (ticks_to_text(it.RunTimeTicks) and ("  |  " .. ticks_to_text(it.RunTimeTicks)) or "")
  elseif playable then
    sub = ticks_to_text(it.RunTimeTicks) or it.Type
  else
    sub = (it.ChildCount and (it.ChildCount .. " Eintraege")) or it.CollectionType or "Ordner"
  end
  local title = it.Name or "?"
  if it.Type == "Episode" and it.IndexNumber then title = it.IndexNumber .. ". " .. title end
  return {
    title = title,
    subtitle = sub,
    id = (playable and "play:" or "item:") .. it.Id,
    kind = playable and "video" or "folder",
    thumb = image_url(sess, it),
  }
end

-- Items unter einem Parent laden (seitenweise). sort steuert die Reihenfolge je nach Inhalt.
local function list_items(sess, parent_id, start)
  start = start or 0
  -- herausfinden, worum es sich handelt, um sinnvoll zu sortieren
  local info = parent_id and select(1, api_get(sess, "/Users/" .. sess.uid .. "/Items/" .. parent_id))
  local sort = "SortName"
  local extra = ""
  if info then
    if info.Type == "Series" then
      -- Staffeln dieser Serie
      local data, err = api_get(sess, string.format(
        "/Shows/%s/Seasons?UserId=%s&Fields=ChildCount,PrimaryImageAspectRatio", parent_id, sess.uid))
      if not data then return nil, err end
      local out = {}
      for _, it in ipairs(data.Items or {}) do out[#out + 1] = item_to_entry(sess, it) end
      return out
    elseif info.Type == "Season" then
      -- Folgen dieser Staffel, in Reihenfolge
      local data, err = api_get(sess, string.format(
        "/Shows/%s/Episodes?UserId=%s&SeasonId=%s&Fields=Overview,RunTimeTicks",
        info.SeriesId or "", sess.uid, parent_id))
      if not data then return nil, err end
      local out = {}
      for _, it in ipairs(data.Items or {}) do out[#out + 1] = item_to_entry(sess, it) end
      return out
    elseif info.CollectionType == "movies" or info.CollectionType == "boxsets" then
      sort = "SortName"
    elseif info.CollectionType == "music" then
      sort = "SortName"
    end
  end

  local path = string.format(
    "/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d&SortBy=%s&SortOrder=Ascending" ..
    "&Fields=ChildCount,ProductionYear,RunTimeTicks,PrimaryImageAspectRatio&ImageTypeLimit=1%s",
    sess.uid, parent_id or "", start, PAGE, sort, extra)
  local data, err = api_get(sess, path)
  if not data then return nil, err end
  local out = {}
  for _, it in ipairs(data.Items or {}) do out[#out + 1] = item_to_entry(sess, it) end
  local total = data.TotalRecordCount or #out
  if start + PAGE < total then
    out[#out + 1] = { title = "Weitere laden (" .. (total - start - PAGE) .. ")",
                      id = string.format("page:%s:%d", parent_id or "", start + PAGE), kind = "more" }
  end
  if #out == 0 then return nil, "Dieser Ordner ist leer" end
  return out
end

-- Ansichten (oberste Ebene: Filme, Serien, Musik, ... je nach Server)
local function list_views(sess)
  local data, err = api_get(sess, "/Users/" .. sess.uid .. "/Views")
  if not data then return nil, err end
  local out = {}
  for _, it in ipairs(data.Items or {}) do out[#out + 1] = item_to_entry(sess, it) end
  out[#out + 1] = { title = "Suchen ...", subtitle = "im gesamten Jellyfin-Server", id = "search", kind = "search" }
  out[#out + 1] = { title = "Abmelden", subtitle = "Zugangsdaten vom Geraet loeschen", id = "logout", kind = "folder" }
  if #out == 0 then return nil, "Keine Bibliotheken sichtbar" end
  return out
end

local function do_search(sess, query)
  local path = string.format(
    "/Users/%s/Items?searchTerm=%s&Recursive=true&Limit=60&IncludeItemTypes=Movie,Series,Episode,MusicVideo,Video" ..
    "&Fields=ProductionYear,RunTimeTicks,ChildCount&ImageTypeLimit=1",
    sess.uid, vs.urlencode(query))
  local data, err = api_get(sess, path)
  if not data then return nil, err end
  local out = {}
  for _, it in ipairs(data.Items or {}) do out[#out + 1] = item_to_entry(sess, it) end
  if #out == 0 then return nil, "Keine Treffer fuer \"" .. query .. "\"" end
  return out
end

-- ---------------------------------------------------------------- Wiedergabe

-- Was die Vita kann (Geraeteprofil). Bewusst streng: der Player oeffnet MP4/MOV/TS,
-- dekodiert H.264 (8 Bit) bis 1280x720 in Hardware und AAC/MP3/AC3 als Ton.
local H264_CONDITIONS = {
  { Condition = "LessThanEqual", Property = "Width", Value = tostring(MAXW), IsRequired = false },
  { Condition = "LessThanEqual", Property = "Height", Value = tostring(MAXH), IsRequired = false },
  { Condition = "LessThanEqual", Property = "VideoBitDepth", Value = "8", IsRequired = false },
  { Condition = "LessThanEqual", Property = "VideoLevel", Value = "41", IsRequired = false },
  { Condition = "EqualsAny", Property = "VideoProfile", Value = "high|main|baseline|constrained baseline", IsRequired = false },
  { Condition = "NotEquals", Property = "IsInterlaced", Value = "true", IsRequired = false },
}
local DEVICE_PROFILE = {
  Name = "PS Vita (VitaStream)",
  MaxStreamingBitrate = 8000000, MaxStaticBitrate = 8000000, MusicStreamingTranscodingBitrate = 192000,
  DirectPlayProfiles = {
    { Container = "mp4,m4v,mov", Type = "Video", VideoCodec = "h264", AudioCodec = "aac,mp3,ac3" },
    { Container = "ts,mpegts", Type = "Video", VideoCodec = "h264", AudioCodec = "aac,mp3,ac3,mp2" },
    { Container = "mp3", Type = "Audio", AudioCodec = "mp3" },
    { Container = "aac,m4a,m4b", Type = "Audio", AudioCodec = "aac" },
  },
  TranscodingProfiles = {
    { Container = "ts", Type = "Video", VideoCodec = "h264", AudioCodec = "aac", Protocol = "hls",
      Context = "Streaming", MaxAudioChannels = "2", BreakOnNonKeyFrames = true, MinSegments = 1 },
    { Container = "mp3", Type = "Audio", AudioCodec = "mp3", Protocol = "http", Context = "Streaming", MaxAudioChannels = "2" },
  },
  CodecProfiles = {
    { Type = "Video", Codec = "h264", Conditions = H264_CONDITIONS },
    { Type = "VideoAudio", Conditions = { { Condition = "LessThanEqual", Property = "AudioChannels", Value = "6", IsRequired = false } } },
  },
  -- Text-Untertitel liefert der Server als WebVTT, Bild-Untertitel (PGS, DVD) brennt er beim Umwandeln ein
  SubtitleProfiles = {
    { Format = "vtt", Method = "External" },
    { Format = "srt", Method = "External" },
  },
}

local function api_post(sess, path, body)
  local url = sess.server .. path
  local resp, status = vs.http_post(url, json.encode(body),
    auth_header(sess.token) .. "\nContent-Type: application/json\nAccept: application/json")
  if not resp then return nil, status end
  if status == 401 then return nil, "Nicht angemeldet (Token abgelaufen) - bitte neu anmelden" end
  if status and status >= 400 then return nil, "Server-Fehler HTTP " .. status end
  local ok, data = pcall(json.decode, resp)
  if not ok then return nil, "Ungueltige Antwort vom Server" end
  return data
end

local function with_key(sess, url)
  if not url:find("api_key=", 1, true) and not url:find("ApiKey=", 1, true) then
    url = url .. (url:find("?", 1, true) and "&" or "?") .. "api_key=" .. sess.token
  end
  return url
end

-- Stream ermitteln: Server entscheidet anhand des Geraeteprofils
local function stream_for(sess, item_id)
  local pb, err = api_post(sess, string.format("/Items/%s/PlaybackInfo?UserId=%s&IsPlayback=true&AutoOpenLiveStream=true",
                                               item_id, sess.uid),
                           { DeviceProfile = DEVICE_PROFILE, MaxStreamingBitrate = 8000000, UserId = sess.uid,
                             EnableDirectPlay = true, EnableDirectStream = false, EnableTranscoding = true,
                             AllowVideoStreamCopy = true, AllowAudioStreamCopy = true,
                             AlwaysBurnInSubtitleWhenTranscoding = false })
  if not pb then return nil, err end
  if pb.ErrorCode then return nil, "Server: " .. tostring(pb.ErrorCode) end
  local src = pb.MediaSources and pb.MediaSources[1]
  if not src then return nil, "Server lieferte keine Medienquelle" end

  -- externe Text-Untertitel (WebVTT vom Server)
  local subs = {}
  for _, st in ipairs(src.MediaStreams or {}) do
    if st.Type == "Subtitle" and st.DeliveryUrl and st.DeliveryMethod == "External" and #subs < 4 then
      subs[#subs + 1] = { label = st.DisplayTitle or st.Language or ("Spur " .. tostring(st.Index)),
                          url = with_key(sess, sess.server .. st.DeliveryUrl) }
    end
  end
  local res = { subtitles = (#subs > 0) and subs or nil }

  if src.SupportsDirectPlay then
    local kind = (src.MediaStreams and #src.MediaStreams > 0 and src.MediaStreams[1].Type == "Audio") and "Audio" or "Videos"
    for _, st in ipairs(src.MediaStreams or {}) do if st.Type == "Video" then kind = "Videos" end end
    res.url = string.format("%s/%s/%s/stream?static=true&mediaSourceId=%s&playSessionId=%s&api_key=%s",
                            sess.server, kind, item_id, vs.urlencode(src.Id or item_id),
                            vs.urlencode(pb.PlaySessionId or ""), sess.token)
    if src.Container then vs.log("Direkt: " .. src.Container) end
    return res
  end
  if src.TranscodingUrl then
    res.url = with_key(sess, sess.server .. src.TranscodingUrl)
    vs.log("Server wandelt um (" .. tostring(src.TranscodeReasons or src.TranscodingSubProtocol or "") .. ")")
    return res
  end
  return nil, "Der Server kann dieses Video weder direkt liefern noch umwandeln (Transkodierung deaktiviert?)"
end

-- ---------------------------------------------------------------- Quelle

local function setup_items(msg)
  local last = last_login()
  local items = {}
  if msg then items[#items + 1] = { title = msg, subtitle = "Bitte (erneut) anmelden", id = "nop", kind = "folder" } end
  items[#items + 1] = { title = "1. Server-Adresse eingeben ...",
                        subtitle = last.server and ("zuletzt: " .. last.server) or "z. B. 192.168.1.50:8096 oder https://jellyfin.example.de",
                        id = "login_server", kind = "search" }
  if last.server and last.user then
    items[#items + 1] = { title = "Als " .. last.user .. " anmelden (Passwort eingeben) ...",
                          subtitle = last.server, id = "login_pass:" .. last.server .. "|" .. last.user, kind = "search" }
  end
  return items
end

local function root()
  local sess, err = ensure_session()
  if sess then return list_views(sess) end
  if err == "setup" then return setup_items(nil) end
  return setup_items("Verbindung fehlgeschlagen: " .. tostring(err))
end

return {
  name = "Jellyfin",
  save_ref = true,   -- Stream-Adressen laufen ab: in Playlists Verweis speichern
  description = "Eigener Jellyfin-Server: Mediathek durchsuchen und abspielen",

  browse = function(id)
    if id == nil then return root() end
    if id == "nop" then return nil, "Bitte unten anmelden" end
    if id == "logout" then
      local sess = mem_session or read_session()
      if sess then vs.http_post(sess.server .. "/Sessions/Logout", "", auth_header(sess.token)) end
      vs.secret_set(SECRET, nil)
      mem_session, checked = nil, false
      return setup_items("Abgemeldet")
    end
    local sess, err = ensure_session()
    if not sess then return nil, err == "setup" and "Bitte zuerst anmelden" or err end
    local item = id:match("^item:(.+)$")
    if item then return list_items(sess, item, 0) end
    local pid, start = id:match("^page:([^:]*):(%d+)$")
    if pid then return list_items(sess, pid ~= "" and pid or nil, tonumber(start)) end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(query, ctx)
    local q = trim(query or "")
    if q == "" then return nil, "Bitte etwas eingeben" end
    -- Anmeldung Schritt 1: Server
    if ctx == "login_server" then
      local base = q:gsub("/+$", "")
      if not base:match("^https?://") then base = "http://" .. base end
      local candidates = { base }
      if not base:match("^https?://[^/]+:%d+") then candidates[2] = base .. ":8096" end   -- Standard-Port
      local server, pub, last_err
      for _, c in ipairs(candidates) do
        local info, status = vs.http_get(c .. "/System/Info/Public", "Accept: application/json")
        local ok, p = pcall(json.decode, info or "")
        if info and ok and type(p) == "table" and (p.ServerName or p.Version) then server, pub = c, p; break end
        last_err = status
      end
      if not server then
        return nil, "Unter " .. base .. " antwortet kein Jellyfin-Server (" .. tostring(last_err) .. ")"
      end
      return {
        { title = "Server: " .. tostring(pub.ServerName or "Jellyfin"), subtitle = server .. "  |  Version " .. tostring(pub.Version or "?"),
          id = "nop", kind = "folder" },
        { title = "2. Benutzername eingeben ...", subtitle = "Konto auf diesem Server", id = "login_user:" .. server, kind = "search" },
      }
    end
    -- Schritt 2: Benutzer
    local srv = ctx and ctx:match("^login_user:(.+)$")
    if srv then
      return {
        { title = "Benutzer: " .. q, subtitle = srv, id = "nop", kind = "folder" },
        { title = "3. Passwort eingeben ...", subtitle = "wird nicht gespeichert", id = "login_pass:" .. srv .. "|" .. q, kind = "search" },
      }
    end
    -- Schritt 3: Passwort -> anmelden
    local srv2, user = (ctx or ""):match("^login_pass:(.-)|(.+)$")
    if srv2 then
      local sess, err = login(srv2, user, query or "")
      if not sess then return nil, err end
      mem_session, checked = sess, true
      return list_views(sess)
    end
    local sess, err = ensure_session()
    if not sess then return nil, err == "setup" and "Bitte zuerst anmelden" or err end
    return do_search(sess, q)
  end,

  resolve = function(item)
    local sess, err = ensure_session()
    if not sess then return nil, err == "setup" and "Bitte zuerst anmelden" or err end
    local item_id = (item.id or ""):match("^play:(.+)$") or item.id
    local ok, res, e2 = pcall(stream_for, sess, item_id)
    if not ok then return nil, "Wiedergabe fehlgeschlagen: " .. tostring(res) end
    if not res then return nil, e2 or "Kein Stream verfuegbar" end
    return res
  end,
}
