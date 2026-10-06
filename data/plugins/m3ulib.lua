-- Gemeinsame M3U-Funktionen fuer m3u.lua und finder.lua (keine eigene Quelle).
local M = {}

local cache = {}          -- url -> { entries, time }
local CACHE_MAX = 6

local function attr(s, key)
  return s:match(key .. '="([^"]*)"')
end

-- erste http(s)-Adresse aus "a.xml.gz,b.xml"
local function first_url(s)
  if not s then return nil end
  for u in s:gmatch("[^,%s]+") do
    if u:match("^https?://") then return u end
  end
  return nil
end

-- Zerlegt M3U-Text in Eintraege { title, url, group, logo, headers, tvg_id, tvg_name }
-- entries.tvg_url = Programmfuehrer-Adresse aus dem Kopf (url-tvg / x-tvg-url)
function M.parse(text)
  local entries = {}
  local cur, opts = nil, {}
  for raw in text:gmatch("[^\r\n]+") do
    local line = raw:gsub("^%s+", ""):gsub("%s+$", "")
    if line:match("^#EXTM3U") then
      entries.tvg_url = entries.tvg_url or first_url(attr(line, "url%-tvg") or attr(line, "x%-tvg%-url"))
    elseif line:match("^#EXTINF") then
      local meta, title = line:match("^#EXTINF:[^ ,]*(.-),(.*)$")
      meta = meta or ""
      cur = {
        title = (title and #title > 0) and title or "Ohne Titel",
        group = attr(meta, "group%-title") or "",
        logo = attr(meta, "tvg%-logo"),
        tvg_id = attr(meta, "tvg%-id"),
        tvg_name = attr(meta, "tvg%-name"),
      }
      if cur.tvg_id == "" then cur.tvg_id = nil end
      if cur.tvg_name == "" then cur.tvg_name = nil end
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
      e.idx = #entries
      cur, opts = nil, {}
    end
  end
  return entries
end

-- Laedt eine Playlist (URL oder "file:name"), mit kleinem Cache fuer Online-Listen
function M.forget(url) cache[url] = nil end

-- Programmfuehrer starten: Adresse der Liste, sonst die aus den Einstellungen
function M.use_epg(entries, fallback)
  if not vs.epg_load then return end
  vs.epg_load((entries and entries.tvg_url) or fallback or "")
end

-- epg (optional): Programmfuehrer-Adresse, falls die Liste selbst keine nennt
function M.load(url, epg)
  if url:match("^file:") then
    local text = vs.read_file(url:sub(6))
    if not text then return nil, "Datei nicht gefunden: " .. url:sub(6) end
    local entries = M.parse(text)
    M.use_epg(entries, epg)
    return entries
  end
  if cache[url] then M.use_epg(cache[url].entries, epg); return cache[url].entries end
  vs.log("Lade Liste ...")
  local text, status = vs.http_get(url)
  if not text then return nil, status end
  if status and status >= 400 then return nil, "HTTP " .. status end
  vs.log("Lese Sender ...")
  local entries = M.parse(text)
  M.use_epg(entries, epg)
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

-- Laufende/naechste Sendung (Programmfuehrer); nil, wenn unbekannt
local function epg_of(t)
  if not vs.epg_now then return nil end
  return vs.epg_now(t.tvg_id, t.tvg_name or t.title)
end

function M.now_line(t)
  local g = epg_of(t)
  if not g then return nil end
  if g.now then return "Jetzt: " .. g.now.title .. " (bis " .. g.now.to .. ")" end
  if g.next then return "Ab " .. g.next.from .. ": " .. g.next.title end
  return nil
end

-- info(item) fuer die App: "Jetzt 20:15-21:45 Tatort (40 %) | Danach 21:45 Tagesthemen"
function M.info(item)
  local g = epg_of(item)
  if not g then return nil end
  local parts = {}
  if g.now then
    parts[#parts + 1] = string.format("Jetzt %s-%s %s (%d %%)", g.now.from, g.now.to, g.now.title, g.now.percent or 0)
  end
  if g.next then parts[#parts + 1] = "Danach " .. g.next.from .. " " .. g.next.title end
  if #parts == 0 then return nil end
  return table.concat(parts, "  |  ")
end

-- Eintrag fuer die App-Liste
function M.item(e, subtitle)
  local now = M.now_line(e)
  local sub = subtitle or e.group
  if now then sub = (sub and #sub > 0) and (now .. "  -  " .. sub) or now end
  return {
    title = e.title, subtitle = sub, thumb = e.logo,
    id = e.url, kind = "video", url = e.url, headers = e.headers, idx = e.idx,
    tvg_id = e.tvg_id, tvg_name = e.tvg_name,
  }
end

-- Header-Tabelle -> Text fuer vs.probe/vs.http_get
function M.header_text(h)
  if not h then return nil end
  local t = {}
  for k, v in pairs(h) do t[#t + 1] = k .. ": " .. v end
  return table.concat(t, "\n")
end

-- Eintraege wieder als M3U-Text
function M.serialize(entries)
  local out = { entries.tvg_url and ('#EXTM3U url-tvg="' .. entries.tvg_url .. '"') or "#EXTM3U" }
  for _, e in ipairs(entries) do
    local attrs = {}
    if e.tvg_id then attrs[#attrs + 1] = 'tvg-id="' .. e.tvg_id .. '"' end
    if e.tvg_name then attrs[#attrs + 1] = 'tvg-name="' .. e.tvg_name .. '"' end
    if e.logo then attrs[#attrs + 1] = 'tvg-logo="' .. e.logo .. '"' end
    if e.group and #e.group > 0 then attrs[#attrs + 1] = 'group-title="' .. e.group .. '"' end
    out[#out + 1] = "#EXTINF:-1" .. (#attrs > 0 and (" " .. table.concat(attrs, " ")) or "") .. "," .. e.title
    if e.headers then
      if e.headers.Referer then out[#out + 1] = "#EXTVLCOPT:http-referrer=" .. e.headers.Referer end
      if e.headers["User-Agent"] then out[#out + 1] = "#EXTVLCOPT:http-user-agent=" .. e.headers["User-Agent"] end
    end
    out[#out + 1] = e.url
  end
  return table.concat(out, "\n") .. "\n"
end

function M.save(file, entries)
  return vs.write_file(file, M.serialize(entries))
end

-- Prueft alle Eintraege, liefert die funktionierenden und die Zahl der defekten
function M.check_all(entries)
  local alive, dead = {}, 0
  for i, e in ipairs(entries) do
    vs.log(string.format("Pruefe %d/%d: %s (%d defekt)", i, #entries, e.title, dead))
    local ok = vs.probe(e.url, M.header_text(e.headers))
    if ok then alive[#alive + 1] = e else dead = dead + 1 end
  end
  return alive, dead
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
