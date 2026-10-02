-- Gemeinsame M3U-Funktionen fuer m3u.lua und finder.lua (keine eigene Quelle).
local M = {}

local cache = {}          -- url -> { entries, time }
local CACHE_MAX = 6

local function attr(s, key)
  return s:match(key .. '="([^"]*)"')
end

-- Zerlegt M3U-Text in Eintraege { title, url, group, logo, headers }
function M.parse(text)
  local entries = {}
  local cur, opts = nil, {}
  for raw in text:gmatch("[^\r\n]+") do
    local line = raw:gsub("^%s+", ""):gsub("%s+$", "")
    if line:match("^#EXTINF") then
      local meta, title = line:match("^#EXTINF:[^ ,]*(.-),(.*)$")
      meta = meta or ""
      cur = {
        title = (title and #title > 0) and title or "Ohne Titel",
        group = attr(meta, "group%-title") or "",
        logo = attr(meta, "tvg%-logo"),
      }
      local ref, ua = attr(meta, "http%-referrer"), attr(meta, "http%-user%-agent")
      opts = {}
      if ref and #ref > 0 then opts.Referer = ref end
      if ua and #ua > 0 then opts["User-Agent"] = ua end
    elseif line:match("^#EXTVLCOPT:") then
      local k, v = line:match("^#EXTVLCOPT:([^=]+)=(.*)$")
      if k == "http-referrer" or k == "http-referer" then opts.Referer = v
      elseif k == "http-user-agent" then opts["User-Agent"] = v end
    elseif line:match("^#EXTGRP:") and cur then
      cur.group = line:sub(9)
    elseif line ~= "" and not line:match("^#") then
      local e = cur or { title = line:match("([^/]+)$") or line, group = "" }
      e.url = line
      if next(opts) then e.headers = opts end
      if e.logo == "" then e.logo = nil end
      entries[#entries + 1] = e
      cur, opts = nil, {}
    end
  end
  return entries
end

-- Laedt eine Playlist (URL oder "file:name"), mit kleinem Cache fuer Online-Listen
function M.load(url)
  if url:match("^file:") then
    local text = vs.read_file(url:sub(6))
    if not text then return nil, "Datei nicht gefunden: " .. url:sub(6) end
    return M.parse(text)
  end
  if cache[url] then return cache[url].entries end
  vs.log("Lade Liste ...")
  local text, status = vs.http_get(url)
  if not text then return nil, status end
  if status and status >= 400 then return nil, "HTTP " .. status end
  vs.log("Lese Sender ...")
  local entries = M.parse(text)
  -- Cache begrenzen
  local n, oldest, oldest_t = 0, nil, math.huge
  for k, v in pairs(cache) do
    n = n + 1
    if v.time < oldest_t then oldest, oldest_t = k, v.time end
  end
  if n >= CACHE_MAX and oldest then cache[oldest] = nil end
  M.clock = (M.clock or 0) + 1
  cache[url] = { entries = entries, time = M.clock }
  return entries
end

-- Eintrag fuer die App-Liste
function M.item(e, subtitle)
  return {
    title = e.title, subtitle = subtitle or e.group, thumb = e.logo,
    id = e.url, kind = "video", url = e.url, headers = e.headers,
  }
end

-- Seitenweise Ausgabe: liefert Items ab offset, haengt "Weitere laden" an
function M.page(list, offset, size, more_id_fn, subtitle_fn)
  local items = {}
  local last = math.min(#list, offset + size)
  for i = offset + 1, last do
    items[#items + 1] = M.item(list[i], subtitle_fn and subtitle_fn(list[i]) or nil)
  end
  if last < #list then
    items[#items + 1] = {
      title = string.format("Weitere laden (%d von %d)", last, #list),
      kind = "more", id = more_id_fn(last),
    }
  end
  return items
end

-- Als resolve-Funktion der Quellen verwenden (Name bewusst nicht 'resolve',
-- sonst haelt die App diese Bibliothek fuer eine Quelle)
function M.resolve_item(item)
  return { url = item.url or item.id, headers = item.headers }
end

return M
