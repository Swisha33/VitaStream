-- Audiothek: Internetradio und Podcasts (getrennt von der Video-Mediathek)
--
-- Radio:    radio-browser.info - offenes, gemeinschaftlich gepflegtes Senderverzeichnis
--           (nur Formate, die die Vita abspielt: MP3, AAC, HLS)
-- Podcasts: Podcast-Verzeichnis von Apple (Suche + Charts) und die offenen RSS-Feeds der
--           Podcasts selbst - darueber auch alle Podcasts von ARD, ZDF, Deutschlandfunk & Co.

local json = require("json")

local RADIO_SERVERS = { "https://de1.api.radio-browser.info", "https://fi1.api.radio-browser.info",
                        "https://at1.api.radio-browser.info" }
local UA = "User-Agent: VitaStream/0.9 (PS Vita homebrew)"
local PLAYABLE = { MP3 = true, AAC = true, ["AAC+"] = true, MPEG = true, HLS = true }
local RADIO_PAGE = 60

local GENRES = {
  { "Pop", "pop" }, { "Rock", "rock" }, { "Nachrichten & Info", "news" }, { "Schlager", "schlager" },
  { "Klassik", "classical" }, { "Jazz", "jazz" }, { "Elektronisch", "electronic" }, { "Hip-Hop", "hip-hop" },
  { "Oldies & 80er", "80s" }, { "Kinder", "kids" }, { "Comedy", "comedy" }, { "Sport", "sport" },
}
local COUNTRIES = {
  { "Deutschland", "DE" }, { "Oesterreich", "AT" }, { "Schweiz", "CH" }, { "Kroatien", "HR" },
  { "Grossbritannien", "GB" }, { "USA", "US" }, { "Frankreich", "FR" }, { "Spanien", "ES" },
  { "Italien", "IT" }, { "Japan", "JP" },
}
local PUBLIC_PODCASTS = { "Deutschlandfunk", "ARD", "ZDF", "WDR", "NDR", "BR Bayern 2", "SWR", "MDR",
                          "hr", "rbb", "Deutsche Welle", "KiKA Kinder Hoerspiel", "ORF", "SRF" }

local function enc(s) return (tostring(s):gsub("[^%w%-_%.~]", function(c) return string.format("%%%02X", c:byte()) end)) end
local function dec(s) return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end
local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

-- ---------------------------------------------------------------- Radio

local function radio_get(path)
  local last
  for _, srv in ipairs(RADIO_SERVERS) do
    local body, status = vs.http_get(srv .. path, UA)
    if body and (not status or status < 400) then
      local ok, data = pcall(json.decode, body)
      if ok and type(data) == "table" then return data end
      last = "Antwort unlesbar"
    else
      last = tostring(status)
    end
  end
  return nil, "Radioverzeichnis nicht erreichbar (" .. tostring(last) .. ")"
end

local function station_items(list, more_id)
  local items = {}
  for _, st in ipairs(list) do
    local codec = (st.codec or ""):upper()
    local url = (st.url_resolved and #st.url_resolved > 0) and st.url_resolved or st.url
    if url and (PLAYABLE[codec] or url:lower():find("%.m3u8")) then
      local sub = {}
      if st.countrycode and #st.countrycode > 0 then sub[#sub + 1] = st.countrycode end
      if codec ~= "" then sub[#sub + 1] = codec .. ((tonumber(st.bitrate) or 0) > 0 and (" " .. st.bitrate .. " kbit/s") or "") end
      local tags = (st.tags or ""):gsub(",", ", ")
      if #tags > 0 then sub[#sub + 1] = tags:sub(1, 60) end
      items[#items + 1] = {
        title = trim(st.name or "?"), subtitle = table.concat(sub, "  |  "),
        id = url, url = url, kind = "video", live = true,
        thumb = (st.favicon and st.favicon:match("^https?://") and not st.favicon:lower():find("%.ico")) and st.favicon or nil,
      }
    end
  end
  if more_id and #list >= RADIO_PAGE then
    items[#items + 1] = { title = "Weitere Sender", kind = "more", id = more_id }
  end
  return items
end

local function radio_search(params, offset, more_prefix)
  local q = "/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=" .. RADIO_PAGE ..
            "&offset=" .. offset .. params
  local list, err = radio_get(q)
  if not list then return nil, err end
  local items = station_items(list, more_prefix and (more_prefix .. (offset + RADIO_PAGE)) or nil)
  if #items == 0 then return nil, "Keine abspielbaren Sender gefunden" end
  return items
end

-- ---------------------------------------------------------------- Podcasts

local function podcast_items(results)
  local items = {}
  for _, r in ipairs(results) do
    local feed = r.feedUrl
    if feed and #feed > 0 then
      local sub = { r.artistName or "" }
      if r.primaryGenreName then sub[#sub + 1] = r.primaryGenreName end
      if r.trackCount then sub[#sub + 1] = r.trackCount .. " Folgen" end
      items[#items + 1] = { title = r.collectionName or r.trackName or "?", subtitle = table.concat(sub, "  |  "),
                            id = "feed:" .. feed, kind = "folder", thumb = r.artworkUrl600 or r.artworkUrl100 }
    end
  end
  return items
end

local function podcast_search(term)
  local body, status = vs.http_get("https://itunes.apple.com/search?media=podcast&entity=podcast&country=DE&limit=40&term=" .. enc(term), UA)
  if not body then return nil, "Podcast-Verzeichnis nicht erreichbar: " .. tostring(status) end
  local ok, data = pcall(json.decode, body)
  if not ok or type(data) ~= "table" then return nil, "Antwort unlesbar" end
  local items = podcast_items(data.results or {})
  if #items == 0 then return nil, "Keine Podcasts gefunden fuer: " .. term end
  return items
end

local function podcast_charts()
  local body, status = vs.http_get("https://rss.applemarketingtools.com/api/v2/de/podcasts/top/50/podcasts.json", UA)
  if not body then return nil, "Charts nicht erreichbar: " .. tostring(status) end
  local ok, data = pcall(json.decode, body)
  local res = ok and type(data) == "table" and data.feed and data.feed.results
  if not res then return nil, "Charts unlesbar" end
  -- Feed-Adressen in einem Aufruf nachschlagen
  local ids = {}
  for _, r in ipairs(res) do if r.id then ids[#ids + 1] = r.id end end
  local lb = vs.http_get("https://itunes.apple.com/lookup?entity=podcast&id=" .. table.concat(ids, ","), UA)
  local ok2, ld = pcall(json.decode, lb or "")
  local feeds = {}
  if ok2 and type(ld) == "table" then
    for _, x in ipairs(ld.results or {}) do if x.collectionId then feeds[tostring(x.collectionId)] = x end end
  end
  local merged = {}
  for _, r in ipairs(res) do
    local x = feeds[tostring(r.id)]
    if x then merged[#merged + 1] = x end
  end
  local items = podcast_items(merged)
  if #items == 0 then return nil, "Keine Charts verfuegbar" end
  return items
end

local function xml_text(s)
  if not s then return nil end
  s = s:gsub("^%s*<!%[CDATA%[(.-)%]%]>%s*$", "%1")
  return trim(vs.html_unescape(s:gsub("<[^>]+>", "")))
end

local function fmt_duration(d)
  if not d or d == "" then return nil end
  if d:find(":") then return d end
  local s = tonumber(d)
  if not s then return nil end
  if s >= 3600 then return string.format("%d:%02d:%02d", s // 3600, (s // 60) % 60, s % 60) end
  return string.format("%d:%02d", s // 60, s % 60)
end

local MAX_EPISODES = 300

local function feed_episodes(feed)
  local xml, status = vs.http_get(feed, UA)
  if not xml then return nil, "Feed nicht erreichbar: " .. tostring(status) end
  local channel_img = xml:match("<itunes:image[^>]-href=\"([^\"]+)\"") or xml:match("<image>.-<url>(.-)</url>")
  local items = {}
  for item in xml:gmatch("<item[%s>].-</item>") do
    local enclosure = item:match("<enclosure[^>]+>")
    local url = enclosure and (enclosure:match('url="([^"]+)"') or enclosure:match("url='([^']+)'"))
    local typ = enclosure and (enclosure:match('type="([^"]+)"') or "") or ""
    if url and not typ:find("video/x%-m4v") then
      url = vs.html_unescape(url)
      local sub = {}
      local date = xml_text(item:match("<pubDate>(.-)</pubDate>"))
      if date then sub[#sub + 1] = (date:gsub("%s+%d%d:%d%d:%d%d.*$", "")) end
      local d = fmt_duration(xml_text(item:match("<itunes:duration>(.-)</itunes:duration>")))
      if d then sub[#sub + 1] = d end
      if typ:find("video") then sub[#sub + 1] = "Video" end
      items[#items + 1] = {
        title = xml_text(item:match("<title>(.-)</title>")) or "Folge",
        subtitle = table.concat(sub, "  |  "), id = url, url = url, kind = "video",
        thumb = item:match("<itunes:image[^>]-href=\"([^\"]+)\"") or channel_img,
      }
      if #items >= MAX_EPISODES then break end
    end
  end
  if #items == 0 then return nil, "Keine abspielbaren Folgen im Feed" end
  return items
end

-- ---------------------------------------------------------------- Quelle

local function folder(title, id, sub) return { title = title, id = id, kind = "folder", subtitle = sub } end

-- ---------------------------------------------------------------- Freie Musik (Menuemusik)
-- Netlabel-Sammlung des Internet Archive: Alben unter Creative-Commons-Lizenz bzw. gemeinfrei.
local FM_BASE = '(licenseurl:*creativecommons* OR licenseurl:*publicdomain*) AND mediatype:(audio)'
local FM_CATS = {
  { "Chiptune & 8-Bit", 'collection:(netlabels) AND subject:(chiptune OR 8bit OR "8-bit")' },
  { "Lounge & Chill", 'collection:(netlabels) AND subject:(chillout OR lounge OR downtempo)' },
  { "Ambient", 'collection:(netlabels) AND subject:(ambient)' },
  { "Elektronisch", 'collection:(netlabels) AND subject:(electronic OR electronica)' },
  { "Jazz", 'collection:(netlabels) AND subject:(jazz)' },
  { "Klassik (gemeinfrei)", '(collection:(musopen) OR (collection:(netlabels) AND subject:(classical)))' },
  { "Videospiel-Stil", 'collection:(netlabels) AND subject:(videogame OR "video game" OR "game music")' },
}
local FM_PAGE = 40

local function license_short(u)
  u = tostring(u or ""):lower()
  if u:find("publicdomain") or u:find("/zero/") then return "gemeinfrei" end
  local kind = u:match("licenses/([%w%-]+)/")
  return kind and ("CC " .. kind:upper()) or "CC"
end

local function fm_albums(ci, page)
  local c = FM_CATS[ci]
  if not c then return nil, "Unbekannt" end
  local q = FM_BASE .. " AND " .. c[2]
  local url = "https://archive.org/advancedsearch.php?q=" .. vs.urlencode(q) ..
              "&fl%5B%5D=identifier&fl%5B%5D=title&fl%5B%5D=creator&fl%5B%5D=licenseurl" ..
              "&sort%5B%5D=downloads%20desc&rows=" .. FM_PAGE .. "&page=" .. (page + 1) .. "&output=json"
  local body, status = vs.http_get(url, UA)
  if not body then return nil, "Internet Archive nicht erreichbar: " .. tostring(status) end
  local ok, data = pcall(json.decode, body)
  local docs = ok and type(data) == "table" and data.response and data.response.docs
  if type(docs) ~= "table" then return nil, "Antwort unlesbar" end
  local items = {}
  for _, d in ipairs(docs) do
    local creator = type(d.creator) == "table" and d.creator[1] or d.creator
    items[#items + 1] = { title = tostring(type(d.title) == "table" and d.title[1] or d.title or d.identifier),
                          subtitle = (creator and (tostring(creator) .. "  |  ") or "") .. license_short(d.licenseurl),
                          id = "fma:" .. d.identifier, kind = "folder",
                          thumb = "https://archive.org/services/img/" .. d.identifier }
  end
  if #docs == FM_PAGE then items[#items + 1] = { title = "Weitere Alben", kind = "more", id = "fmc:" .. ci .. ":" .. (page + 1) } end
  if #items == 0 then return nil, "Keine Alben gefunden" end
  return items
end

local function safe_name(s)
  s = tostring(s):gsub("[^%w%-%._ ]", ""):gsub("%s+", " ")
  s = trim(s)
  return (#s > 0) and s:sub(1, 80) or "Titel"
end

local function fm_tracks(identifier)
  local body, status = vs.http_get("https://archive.org/metadata/" .. identifier, UA)
  local ok, meta = pcall(json.decode, body or "")
  if not ok or type(meta) ~= "table" or type(meta.files) ~= "table" then
    return nil, "Album nicht lesbar (" .. tostring(status) .. ")"
  end
  local md = meta.metadata or {}
  local artist = type(md.creator) == "table" and md.creator[1] or md.creator or "?"
  local lic = license_short(md.licenseurl)
  -- je Titel eine MP3 (VBR bevorzugt, sonst die kleinste)
  local groups, order = {}, {}
  for _, f in ipairs(meta.files) do
    local name = f.name or ""
    if name:lower():match("%.mp3$") then
      local key = ((f.source == "derivative" and f.original) or name):gsub("%.[%w]+$", "")
      local g = groups[key]
      if not g then g = {}; groups[key] = g; order[#order + 1] = key end
      local vbr = tostring(f.format or ""):find("VBR") ~= nil
      if not g.name or (vbr and not g.vbr) then
        g.name, g.vbr, g.title, g.track, g.len = name, vbr, f.title, tonumber(f.track and tostring(f.track):match("%d+")), f.length
      end
    end
  end
  local list = {}
  for _, k in ipairs(order) do list[#list + 1] = groups[k] end
  table.sort(list, function(a, b)
    if a.track and b.track and a.track ~= b.track then return a.track < b.track end
    return a.name:lower() < b.name:lower()
  end)
  local items = {}
  for _, g in ipairs(list) do
    local title = (type(g.title) == "string" and g.title ~= "") and g.title or g.name:gsub("%.mp3$", ""):gsub("[_]+", " ")
    local url = "https://archive.org/download/" .. identifier .. "/" .. (vs.urlencode(g.name):gsub("%%2F", "/"))
    local len = tonumber(g.len)
    items[#items + 1] = {
      title = title, kind = "video", id = url, url = url,
      subtitle = tostring(artist) .. "  |  " .. lic .. (len and string.format("  |  %d:%02d", len // 60, math.floor(len % 60)) or "") ..
                 "  |  Quadrat: als Menuemusik",
      thumb = "https://archive.org/services/img/" .. identifier,
      music = safe_name(tostring(artist) .. " - " .. title) .. ".mp3",
      music_title = title .. " (" .. tostring(artist) .. ", " .. lic .. ")",
    }
  end
  if #items == 0 then return nil, "Keine MP3-Titel in diesem Album" end
  return items
end

return {
  name = "Audiothek (Radio & Podcasts)",
  description = "Internetradio weltweit und Podcasts - getrennt von der Video-Mediathek",

  browse = function(id)
    if id == nil then
      return {
        { title = "Radiosender suchen ...", subtitle = "Name, Stadt oder Genre", id = "radiosearch", kind = "search" },
        { title = "Podcasts suchen ...", subtitle = "Titel oder Anbieter", id = "podsearch", kind = "search" },
        folder("Radio: Beliebt in Deutschland", "rc:DE:0"),
        folder("Radio nach Genre", "rgenres"),
        folder("Radio nach Land", "rcountries"),
        folder("Podcast-Charts", "podcharts", "Top 50 in Deutschland"),
        folder("Podcasts von ARD, ZDF, DLF ...", "podpublic", "oeffentlich-rechtliche Anbieter"),
        folder("Freie Musik fuer die Menuemusik", "fm", "Creative Commons / gemeinfrei - Titel mit Quadrat uebernehmen"),
      }
    end
    if id == "rgenres" then
      local items = {}
      for _, g in ipairs(GENRES) do items[#items + 1] = folder(g[1], "rt:" .. enc(g[2]) .. ":0") end
      return items
    end
    if id == "rcountries" then
      local items = {}
      for _, c in ipairs(COUNTRIES) do items[#items + 1] = folder(c[1], "rc:" .. c[2] .. ":0") end
      return items
    end
    local cc, coff = id:match("^rc:(%u%u):(%d+)$")
    if cc then return radio_search("&countrycode=" .. cc, tonumber(coff), "rc:" .. cc .. ":") end
    local tag, toff = id:match("^rt:(.-):(%d+)$")
    if tag then return radio_search("&tag=" .. tag .. "&tagExact=false", tonumber(toff), "rt:" .. tag .. ":") end
    local nq, noff = id:match("^rn:(.-):(%d+)$")
    if nq then return radio_search("&name=" .. nq, tonumber(noff), "rn:" .. nq .. ":") end

    if id == "fm" then
      local items = {}
      for i, c in ipairs(FM_CATS) do items[#items + 1] = folder(c[1], "fmc:" .. i .. ":0") end
      return items
    end
    local fci, fpage = id:match("^fmc:(%d+):(%d+)$")
    if fci then return fm_albums(tonumber(fci), tonumber(fpage)) end
    local fma = id:match("^fma:(.+)$")
    if fma then return fm_tracks(fma) end

    if id == "podcharts" then return podcast_charts() end
    if id == "podpublic" then
      local items = {}
      for _, p in ipairs(PUBLIC_PODCASTS) do items[#items + 1] = folder(p, "ps:" .. enc(p)) end
      return items
    end
    local ps = id:match("^ps:(.+)$")
    if ps then return podcast_search(dec(ps)) end
    local feed = id:match("^feed:(.+)$")
    if feed then return feed_episodes(feed) end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(query, ctx)
    local q = trim(query or "")
    if q == "" then return nil, "Bitte etwas eingeben" end
    if ctx == "radiosearch" then return radio_search("&name=" .. enc(q), 0, "rn:" .. enc(q) .. ":") end
    return podcast_search(q)
  end,

  resolve = function(item)
    return item.url or item.id
  end,

  actions = function(item)
    if item.music then return { { id = "bgm", label = "Als Menuemusik verwenden" } } end
    return {}
  end,

  action = function(item, id)
    if id == "bgm" and item.music then
      vs.menu_music(item.url or item.id, item.music, item.music_title or item.title)
      return { message = "Wird heruntergeladen und als Menuemusik eingestellt (Fortschritt unten in der Statuszeile)." }
    end
    return nil, "Unbekannte Aktion"
  end,
}
