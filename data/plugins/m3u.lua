-- M3U-Playlists (IPTV-Listen, eigene Sammlungen).
-- Die Playlists stehen in ux0:data/VitaStream/playlists.txt, eine pro Zeile:
--     Name|https://example.org/liste.m3u
--     Lokale Liste|file:meine_liste.m3u      (Datei in ux0:data/VitaStream/)
-- Unterstuetzt #EXTINF mit group-title sowie #EXTVLCOPT:http-referrer / http-user-agent.

local cache = {}   -- index -> geparste Playlist

local function read_playlists()
  local list = {}
  local txt = vs.read_file("playlists.txt") or ""
  for line in txt:gmatch("[^\r\n]+") do
    if not line:match("^%s*#") then
      local name, url = line:match("^%s*(.-)%s*|%s*(%S+)%s*$")
      if name and url and #name > 0 then list[#list + 1] = { name = name, url = url } end
    end
  end
  return list
end

local function attr(s, key)
  return s:match(key .. '="([^"]*)"')
end

local function parse_m3u(text)
  local entries, groups, group_order = {}, {}, {}
  local cur, opts = nil, {}
  for raw in text:gmatch("[^\r\n]+") do
    local line = raw:gsub("^%s+", ""):gsub("%s+$", "")
    if line:match("^#EXTINF") then
      local meta, title = line:match("^#EXTINF:[^ ,]*(.-),(.*)$")
      cur = {
        title = (title and #title > 0) and title or "Ohne Titel",
        group = attr(meta or "", "group%-title") or "Allgemein",
      }
      opts = {}
    elseif line:match("^#EXTVLCOPT:") then
      local k, v = line:match("^#EXTVLCOPT:([^=]+)=(.*)$")
      if k == "http-referrer" then opts.Referer = v
      elseif k == "http-user-agent" then opts["User-Agent"] = v end
    elseif line:match("^#EXTGRP:") and cur then
      cur.group = line:sub(9)
    elseif line ~= "" and not line:match("^#") then
      local e = cur or { title = line:match("([^/]+)$") or line, group = "Allgemein" }
      e.url = line
      e.headers = opts
      entries[#entries + 1] = e
      if not groups[e.group] then
        groups[e.group] = {}
        group_order[#group_order + 1] = e.group
      end
      table.insert(groups[e.group], #entries)
      cur, opts = nil, {}
    end
  end
  return { entries = entries, groups = groups, order = group_order }
end

local function load(idx)
  if cache[idx] then return cache[idx] end
  local pl = read_playlists()[idx]
  if not pl then return nil, "Playlist nicht gefunden" end
  local text, err
  if pl.url:match("^file:") then
    text = vs.read_file(pl.url:sub(6))
    if not text then err = "Datei nicht gefunden: " .. pl.url:sub(6) end
  else
    local status
    text, status = vs.http_get(pl.url)
    if not text then err = status
    elseif status and status >= 400 then text, err = nil, "HTTP " .. status end
  end
  if not text then return nil, err end
  cache[idx] = parse_m3u(text)
  return cache[idx]
end

local function entry_item(idx, n, e)
  return { title = e.title, subtitle = e.group, id = idx .. ":" .. n, kind = "video" }
end

local function list_entries(idx, p, indices)
  local items = {}
  for _, n in ipairs(indices) do items[#items + 1] = entry_item(idx, n, p.entries[n]) end
  return items
end

return {
  name = "M3U-Playlists",
  description = "IPTV- und eigene Listen aus playlists.txt",

  browse = function(id)
    -- Startseite: alle Playlists
    if id == nil then
      local items = {}
      for i, pl in ipairs(read_playlists()) do
        items[#items + 1] = { title = pl.name, subtitle = pl.url, id = "pl:" .. i, kind = "folder" }
      end
      if #items == 0 then return nil, "playlists.txt ist leer" end
      return items
    end

    local pidx = tonumber(id:match("^pl:(%d+)$"))
    if pidx then
      local p, err = load(pidx)
      if not p then return nil, err end
      -- bei mehreren Gruppen erst die Gruppen zeigen
      if #p.order > 1 then
        local items = {}
        for gi, g in ipairs(p.order) do
          items[#items + 1] = { title = g, subtitle = #p.groups[g] .. " Eintraege",
                                id = "grp:" .. pidx .. ":" .. gi, kind = "folder" }
        end
        return items
      end
      return list_entries(pidx, p, p.groups[p.order[1]] or {})
    end

    local gp, gi = id:match("^grp:(%d+):(%d+)$")
    if gp then
      gp, gi = tonumber(gp), tonumber(gi)
      local p, err = load(gp)
      if not p then return nil, err end
      return list_entries(gp, p, p.groups[p.order[gi]] or {})
    end
    return nil, "Unbekannter Eintrag"
  end,

  -- Durchsucht alle Playlists nach Titeln
  search = function(query)
    local q, items = query:lower(), {}
    for i in ipairs(read_playlists()) do
      local p = load(i)
      if p then
        for n, e in ipairs(p.entries) do
          if e.title:lower():find(q, 1, true) then
            items[#items + 1] = entry_item(i, n, e)
            if #items >= 300 then return items end
          end
        end
      end
    end
    return items
  end,

  resolve = function(item)
    local pidx, n = item.id:match("^(%d+):(%d+)$")
    local p = load(tonumber(pidx))
    local e = p and p.entries[tonumber(n)]
    if not e then return nil, "Eintrag nicht mehr vorhanden" end
    return { url = e.url, headers = e.headers }
  end,
}
