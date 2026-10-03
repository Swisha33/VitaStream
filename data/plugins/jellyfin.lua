-- Jellyfin-Client: meldet sich am eigenen Jellyfin-Server an und zeigt die Mediathek
-- so an, wie sie dort aufgebaut ist (Ansichten -> Ordner/Sammlungen -> Serien -> Staffeln -> Folgen).
-- Der Server transkodiert bei Bedarf nach H.264/AAC bis 720p, damit auch HEVC/1080p laeuft.
--
-- Zugangsdaten: Datei  ux0:data/VitaStream/jellyfin.txt  per FTP anlegen:
--     server = http://192.168.1.50:8096
--     user   = maxmustermann
--     pass   = geheim
-- Alternativ ueber die Suche auf der Startseite (Dreieck):  server | user | pass
--
-- Der Zugriffstoken wird in jellyfin_session.txt zwischengespeichert.

local json = require("json")

local CFG   = "jellyfin.txt"
local SESS  = "jellyfin_session.txt"
local CLIENT = "VitaStream"
local DEVICE = "PSVita"
local VERSION = "0.6"
local PAGE = 100            -- Eintraege pro Seite
local MAXH, MAXW = 720, 1280

-- ---------------------------------------------------------------- Hilfen

local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

local function read_cfg()
  local c = {}
  local txt = vs.read_file(CFG)
  if txt then
    for line in txt:gmatch("[^\r\n]+") do
      local k, v = line:match("^%s*([%w_]+)%s*=%s*(.+)$")
      if k then c[k:lower()] = trim(v) end
    end
  end
  if c.server then c.server = c.server:gsub("/+$", "") end
  return c
end

local function read_session()
  local txt = vs.read_file(SESS)
  if not txt then return nil end
  local server = txt:match("server=([^\r\n]+)")
  local token  = txt:match("token=([^\r\n]+)")
  local uid    = txt:match("uid=([^\r\n]+)")
  if server and token and uid then return { server = trim(server), token = trim(token), uid = trim(uid) } end
  return nil
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
  vs.write_file(SESS, string.format("server=%s\ntoken=%s\nuid=%s\n", sess.server, sess.token, sess.uid))
  return sess
end

-- gueltige Sitzung holen: Cache, sonst aus jellyfin.txt anmelden
local function ensure_session()
  local sess = read_session()
  if sess then
    local test = select(1, api_get(sess, "/System/Info"))
    if test then return sess end
  end
  local c = read_cfg()
  if c.server and c.user then
    return login(c.server, c.user, c.pass)
  end
  return nil, "setup"
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

-- Stream-URL bauen: wenn moeglich direkt, sonst HLS-Transkodierung (H.264/AAC, max 720p)
local function stream_for(sess, item_id)
  local info = api_get(sess, "/Users/" .. sess.uid .. "/Items/" .. item_id)
  local source_id = item_id
  local direct = false
  -- PlaybackInfo: echte MediaSource-ID und pruefen, ob direktes Abspielen reicht
  local pb = select(1, api_get(sess, string.format(
    "/Items/%s/PlaybackInfo?UserId=%s&MaxStreamingBitrate=8000000", item_id, sess.uid)))
  local src = pb and pb.MediaSources and pb.MediaSources[1]
  if src then
    source_id = src.Id or source_id
    for _, st in ipairs(src.MediaStreams or {}) do
      if st.Type == "Video" then
        local codec = (st.Codec or ""):lower()
        local h = st.Height or 0
        if (codec == "h264" or codec == "avc") and h > 0 and h <= MAXH then direct = true end
      end
    end
    if src.MediaStreams and #src.MediaStreams == 0 then direct = false end
  end
  local container = (src and src.Container) or "mp4"
  local audio_only = info and (info.Type == "Audio")

  if audio_only then
    return { url = string.format("%s/Audio/%s/stream?static=true&api_key=%s", sess.server, item_id, sess.token) }
  end
  if direct then
    return { url = string.format("%s/Videos/%s/stream?static=true&mediaSourceId=%s&api_key=%s",
                                 sess.server, item_id, source_id, sess.token) }
  end
  -- Transkodierung erzwingen -> laeuft auf der Vita garantiert
  local url = string.format(
    "%s/Videos/%s/master.m3u8?api_key=%s&MediaSourceId=%s&VideoCodec=h264&AudioCodec=aac,mp3" ..
    "&MaxHeight=%d&MaxWidth=%d&VideoBitrate=3000000&AudioBitrate=160000&MaxAudioChannels=2" ..
    "&SegmentContainer=ts&ManifestSubtitles=none&BreakOnNonKeyFrames=true&h264-profile=high&h264-level=41",
    sess.server, item_id, sess.token, source_id, MAXH, MAXW)
  return { url = url }
end

-- ---------------------------------------------------------------- Quelle

local SETUP = {
  { title = "Noch nicht eingerichtet", subtitle = "So meldest du dich an:", id = "nop", kind = "folder" },
  { title = "1) Datei jellyfin.txt anlegen", subtitle = "per FTP (VitaShell) in ux0:data/VitaStream/", id = "nop", kind = "folder" },
  { title = "2) server=, user=, pass= eintragen", subtitle = "z. B. server = http://192.168.1.50:8096", id = "nop", kind = "folder" },
  { title = "Oder: Zugangsdaten hier eingeben", subtitle = "Dreieck - dann: server | benutzer | passwort", id = "login", kind = "search" },
}

local function root()
  local sess, err = ensure_session()
  if sess then return list_views(sess) end
  if err == "setup" then return SETUP end
  -- Zugangsdaten vorhanden, aber Anmeldung schlug fehl
  local items = { { title = "Anmeldung fehlgeschlagen", subtitle = err or "unbekannter Fehler", id = "nop", kind = "folder" } }
  items[#items + 1] = { title = "Zugangsdaten neu eingeben", subtitle = "server | benutzer | passwort", id = "login", kind = "search" }
  return items
end

return {
  name = "Jellyfin",
  description = "Eigener Jellyfin-Server: Mediathek durchsuchen und abspielen",

  browse = function(id)
    if id == nil then return root() end
    if id == "nop" then return nil, "Bitte wie oben beschrieben einrichten" end
    if id == "logout" then
      vs.write_file(SESS, "")
      vs.delete_file(SESS)
      return nil, "Abgemeldet. Startseite erneut oeffnen."
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
    -- Login-Eingabe: server | user | pass
    if (ctx == "login") or q:find("|") then
      local server, user, pass = q:match("^(.-)%s*|%s*(.-)%s*|%s*(.*)$")
      if not server then return nil, "Format: server | benutzer | passwort" end
      local sess, err = login(trim(server), trim(user), pass and trim(pass) or "")
      if not sess then return nil, err end
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
    local ok, res = pcall(stream_for, sess, item_id)
    if not ok then return nil, "Wiedergabe fehlgeschlagen: " .. tostring(res) end
    if not res or not res.url then return nil, "Kein Stream verfuegbar" end
    return res
  end,
}
