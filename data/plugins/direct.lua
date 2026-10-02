-- Direkte URL & Website-Scanner
--  * MP4-/HLS-Link oder Datei (ux0:...) -> direkt abspielen
--  * Domain/Webseite (z. B. "example.org") -> Seite und Unterseiten nach Videos/Streams
--    durchsuchen; die Treffer lassen sich mit Quadrat als Playlist speichern.
-- Verlauf in history_direct.txt.

local m3u = require("m3ulib")
local HISTORY = "history_direct.txt"
local MAX_PAGES = 25      -- Unterseiten pro Scan
local MAX_FRAMES = 10     -- eingebettete Player (iframes)

local MEDIA_EXT = { m3u8 = "HLS", mp4 = "MP4", m3u = "Playlist", ts = "MPEG-TS", mp3 = "Audio", aac = "Audio" }

local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

local function media_type(url)
  local path = url:gsub("[?#].*$", ""):lower()
  local ext = path:match("%.(%w+)$")
  return ext and MEDIA_EXT[ext]
end

local function host_of(u) return (u:match("^https?://([^/?#]+)") or ""):lower():gsub("^www%.", "") end

local function absolute(base, link)
  link = vs.html_unescape(link):gsub("\\/", "/")
  if link:match("^https?://") then return link end
  if link:match("^//") then return (base:match("^(https?:)") or "https:") .. link end
  local origin = base:match("^(https?://[^/?#]+)")
  if not origin then return link end
  if link:sub(1, 1) == "/" then return origin .. link end
  local dir = base:gsub("[?#].*$", ""):match("^(.*/)") or (origin .. "/")
  if dir == "https://" or dir == "http://" then dir = origin .. "/" end
  return dir .. link
end

-- ---------------------------------------------------------------- Verlauf

local function load_history()
  local list = {}
  local txt = vs.read_file(HISTORY)
  if txt then for line in txt:gmatch("[^\r\n]+") do list[#list + 1] = line end end
  return list
end

local function remember(entry)
  local out = { entry }
  for _, u in ipairs(load_history()) do
    if u ~= entry and #out < 30 then out[#out + 1] = u end
  end
  vs.write_file(HISTORY, table.concat(out, "\n") .. "\n")
end

-- ---------------------------------------------------------------- Scanner

local function page_title(html)
  local t = html:match('<meta[^>]-property="og:title"[^>]-content="([^"]+)"')
         or html:match('<meta[^>]-content="([^"]+)"[^>]-property="og:title"')
         or html:match("<title[^>]*>(.-)</title>")
  return t and trim(vs.html_unescape(t):gsub("%s+", " ")) or nil
end

local function page_image(html, base)
  local i = html:match('<meta[^>]-property="og:image"[^>]-content="([^"]+)"')
         or html:match('<meta[^>]-content="([^"]+)"[^>]-property="og:image"')
  return i and absolute(base, i) or nil
end

-- Medien-Links in einer Seite
local function find_media(html, base, out, seen)
  local text = html:gsub("\\/", "/")
  for u in text:gmatch("(https?://[^\"'%s<>\\]+)") do
    if media_type(u) and not seen[u] then seen[u] = true; out[#out + 1] = u end
  end
  for attr_name in ("src href file data%-src"):gmatch("%S+") do
    for u in text:gmatch(attr_name .. '%s*=%s*["\']([^"\']+)["\']') do
      local a = absolute(base, u)
      if media_type(a) and not seen[a] then seen[a] = true; out[#out + 1] = a end
    end
  end
end

local KEYWORDS = { "video", "watch", "folge", "episode", "stream", "live", "film", "play", "media",
                   "sendung", "tv", "clip", "serie", "mediathek", "kanal", "channel" }

local function score(link)
  local l, s = link:lower(), 0
  for _, k in ipairs(KEYWORDS) do if l:find(k, 1, true) then s = s + 1 end end
  return s
end

local SKIP_EXT = { css = 1, js = 1, png = 1, jpg = 1, jpeg = 1, gif = 1, svg = 1, webp = 1, ico = 1,
                   pdf = 1, zip = 1, xml = 1, json = 1, woff = 1, woff2 = 1, ttf = 1, rss = 1 }

local function find_links(html, base, host)
  local links, seen = {}, {}
  for u in html:gmatch('<a[^>]-href%s*=%s*["\']([^"\'#]+)') do
    local a = absolute(base, u)
    local lower = a:lower()
    local ext = lower:gsub("[?#].*$", ""):match("%.(%w+)$")
    if a:match("^https?://") and host_of(a) == host and not seen[a] and a ~= base
       and not (ext and SKIP_EXT[ext]) then
      seen[a] = true
      links[#links + 1] = a
    end
  end
  table.sort(links, function(a, b)
    local sa, sb = score(a), score(b)
    if sa ~= sb then return sa > sb end
    return #a < #b
  end)
  return links
end

local function find_frames(html, base)
  local frames = {}
  for u in html:gmatch('<iframe[^>]-src%s*=%s*["\']([^"\']+)["\']') do
    local a = absolute(base, u)
    if a:match("^https?://") then frames[#frames + 1] = a end
  end
  return frames
end

local function fetch_page(url, referer)
  local html, status, final = vs.http_get(url, referer and ("Referer: " .. referer) or nil)
  if not html or (status and status >= 400) then return nil end
  return html, final or url
end

local function scan(start)
  if not start:match("^https?://") then start = "https://" .. start end
  if not start:match("^https?://[^/]+/") then start = start .. "/" end
  vs.log("Lade " .. start)
  local html, base = fetch_page(start)
  if not html then return nil, "Seite nicht erreichbar: " .. start end

  -- Ist die Adresse selbst eine Playlist?
  if html:match("^%s*#EXTM3U") then
    local entries = m3u.parse(html)
    local items = {}
    for _, e in ipairs(entries) do items[#items + 1] = m3u.item(e) end
    if #items > 0 then return items end
  end

  local host = host_of(base)
  local results, seen_media = {}, {}

  local function add_from(page_html, page_url, depth)
    local media = {}
    find_media(page_html, page_url, media, seen_media)
    local title = page_title(page_html) or page_url
    local image = page_image(page_html, page_url)
    for i, m in ipairs(media) do
      results[#results + 1] = {
        title = (#media > 1) and (title .. " (" .. i .. ")") or title,
        subtitle = media_type(m) .. "  |  " .. host_of(m),
        id = m, url = m, kind = "video",
        headers = { Referer = page_url },
        thumb = image or ("og:" .. page_url),
      }
    end
    -- eingebettete Player
    if depth == 0 then
      local frames = find_frames(page_html, page_url)
      for i = 1, math.min(#frames, MAX_FRAMES) do
        if not vs.is_blocked(frames[i]) then
          local fh, fu = fetch_page(frames[i], page_url)
          if fh then
            local fm = {}
            find_media(fh, fu, fm, seen_media)
            for _, m in ipairs(fm) do
              results[#results + 1] = {
                title = title .. " (Player)", subtitle = media_type(m) .. "  |  " .. host_of(m),
                id = m, url = m, kind = "video", headers = { Referer = fu },
                thumb = image or ("og:" .. page_url),
              }
            end
          end
        end
      end
    end
  end

  add_from(html, base, 0)
  local links = find_links(html, base, host)
  local n = math.min(#links, MAX_PAGES)
  for i = 1, n do
    vs.log(string.format("Durchsuche Seite %d/%d - %d Treffer", i, n, #results))
    local ph, pu = fetch_page(links[i], base)
    if ph then add_from(ph, pu, 0) end
  end

  if #results == 0 then
    return nil, "Keine Videos/Streams auf " .. host .. " gefunden (" .. (n + 1) .. " Seiten durchsucht). " ..
                "Viele Seiten laden Videos erst per JavaScript oder schuetzen sie mit DRM."
  end
  return results
end

-- ---------------------------------------------------------------- Quelle

local function history_item(entry)
  if media_type(entry) or entry:match("^ux0:") then
    return { title = entry, subtitle = media_type(entry) or "Lokale Datei", id = entry, url = entry, kind = "video" }
  end
  return { title = entry, subtitle = "Website durchsuchen", id = "scan:" .. entry, kind = "folder",
           thumb = "og:" .. (entry:match("^https?://") and entry or ("https://" .. entry)) }
end

return {
  name = "Direkte URL & Website-Scanner",
  description = "Link abspielen oder Website nach Videos durchsuchen (Dreieck: Adresse eingeben)",

  browse = function(id)
    if id == nil then
      local items = {}
      for _, e in ipairs(load_history()) do items[#items + 1] = history_item(e) end
      if #items == 0 then return nil, "Noch keine Adressen - mit Dreieck eine Video-URL oder Website eingeben" end
      return items
    end
    local site = id:match("^scan:(.+)$")
    if site then return scan(site) end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(query)
    local q = trim(query)
    if q == "" then return nil, "Bitte eine Adresse eingeben" end
    if q:match("^ux0:") or (q:match("^https?://") and media_type(q)) then
      remember(q)
      return { history_item(q) }
    end
    if not q:match("%.") or q:match("%s") then
      return nil, "Bitte eine Webadresse (z. B. example.org) oder Video-URL eingeben"
    end
    remember(q)
    return scan(q)
  end,

  resolve = function(item)
    local url = item.url or item.id
    if not item.headers then remember(url) end
    return { url = url, headers = item.headers }
  end,
}
