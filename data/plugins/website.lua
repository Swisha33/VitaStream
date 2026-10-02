-- Eigene Websites als Quelle, konfiguriert in ux0:data/VitaStream/sites.txt.
-- Jeder [Abschnitt] wird zu einer eigenen Quelle in der App.
-- Alle Muster sind Lua-Patterns (siehe README, Abschnitt "Eigene Websites").
--
--   [Name der Quelle]
--   start   = https://example.org/videos          -- Seite fuer die Startliste (optional)
--   search  = https://example.org/suche?q={q}      -- Such-URL, {q} wird ersetzt (optional)
--   item    = <a href="(/video/[^"]+)"[^>]*>([^<]+)</a>   -- 2 Captures: Link, Titel
--   embed   = <iframe[^>]+src="([^"]+)"            -- optional: Player-iframe folgen
--   stream  = (https?://[^"'%s]+%.m3u8[^"'%s]*)    -- Stream-Link auf der Detailseite
--   referer = https://example.org/                 -- optional: Referer-Header fuer den Stream
--   description = Text fuer die Quellenliste       -- optional

local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

local function parse_sites()
  local sites, cur = {}, nil
  local txt = vs.read_file("sites.txt") or ""
  for line in txt:gmatch("[^\r\n]+") do
    local l = trim(line)
    if l ~= "" and not l:match("^#") and not l:match("^;") then
      local name = l:match("^%[(.+)%]$")
      if name then
        cur = { name = trim(name) }
        sites[#sites + 1] = cur
      elseif cur then
        local k, v = l:match("^([%w_]+)%s*=%s*(.+)$")
        if k then cur[k:lower()] = v end
      end
    end
  end
  return sites
end

-- relative Links zu absoluten machen
local function absolute(base, link)
  link = vs.html_unescape(link)
  if link:match("^https?://") then return link end
  if link:match("^//") then return (base:match("^(https?:)") or "https:") .. link end
  local origin = base:match("^(https?://[^/]+)")
  if link:match("^/") then return origin .. link end
  local dir = base:match("^(.*/)") or (origin .. "/")
  return dir .. link
end

local function fetch(url, referer)
  local body, status, final = vs.http_get(url, referer and ("Referer: " .. referer) or nil)
  if not body then return nil, status end
  if status and status >= 400 then return nil, "HTTP " .. status .. " bei " .. url end
  return body, final or url
end

local function extract_items(site, page, base)
  if not site.item then return nil, "In sites.txt fehlt 'item' fuer " .. site.name end
  local items, seen = {}, {}
  for link, title in page:gmatch(site.item) do
    local abs = absolute(base, link)
    if not seen[abs] then
      seen[abs] = true
      title = vs.html_unescape((title or abs):gsub("<[^>]+>", ""))
      items[#items + 1] = { title = trim(title), subtitle = abs, id = abs, kind = "video" }
    end
  end
  if #items == 0 then return nil, "Keine Eintraege gefunden - 'item'-Muster pruefen" end
  return items
end

local function find_stream(site, page, base)
  page = page:gsub("\\/", "/")   -- JSON-escapte Links (https:\/\/...) normalisieren
  local pattern = site.stream or "(https?://[^\"'%s]+%.m3u8[^\"'%s]*)"
  local s = page:match(pattern)
  if not s then s = page:match("(https?://[^\"'%s]+%.mp4[^\"'%s]*)") end
  if s then return absolute(base, s) end
  return nil
end

local function make_source(site)
  local src = {
    name = site.name,
    description = site.description or (site.search and "Eigene Website - Dreieck zum Suchen" or "Eigene Website"),
  }

  if site.start then
    src.browse = function(_)
      local page, base = fetch(site.start, site.referer)
      if not page then return nil, base end
      return extract_items(site, page, base)
    end
  end

  if site.search then
    src.search = function(q)
      local url = site.search:gsub("{q}", (vs.urlencode(q):gsub("%%", "%%%%")))
      local page, base = fetch(url, site.referer)
      if not page then return nil, base end
      return extract_items(site, page, base)
    end
  end

  src.resolve = function(item)
    if item.id:match("%.m3u8") or item.id:match("%.mp4") then
      return { url = item.id, headers = site.referer and { Referer = site.referer } or nil }
    end
    local page, base = fetch(item.id, site.referer)
    if not page then return nil, base end

    local stream = find_stream(site, page, base)
    -- optional einem eingebetteten Player (iframe) folgen
    if not stream and site.embed then
      local frame = page:match(site.embed)
      if frame then
        local furl = absolute(base, frame)
        if vs.is_blocked(furl) then return nil, "Player-Host ist durch AdBlock gesperrt" end
        local fpage, fbase = fetch(furl, base)
        if fpage then stream = find_stream(site, fpage, fbase) end
      end
    end
    if not stream then return nil, "Kein Stream-Link gefunden - 'stream'-Muster pruefen" end
    return { url = stream, headers = { Referer = site.referer or base } }
  end

  return src
end

local sources = {}
for _, site in ipairs(parse_sites()) do
  if site.start or site.search then sources[#sources + 1] = make_source(site) end
end
return sources
