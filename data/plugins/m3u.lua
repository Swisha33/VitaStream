-- M3U-Playlists (eigene Listen, IPTV, Favoriten).
-- Die Playlists stehen in ux0:data/VitaStream/playlists.txt, eine pro Zeile:
--     Name|https://example.org/liste.m3u
--     Lokale Liste|file:meine_liste.m3u      (Datei in ux0:data/VitaStream/)
-- Favoriten und eigene Playlists legt die App ueber die Quadrat-Taste an.

local m3u = require("m3ulib")
local PAGE = 100

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

-- Gruppen einer Liste in Reihenfolge des Auftretens
local function groups_of(entries)
  local order, members = {}, {}
  for _, e in ipairs(entries) do
    local g = (e.group and #e.group > 0) and e.group or "Allgemein"
    if not members[g] then members[g] = {}; order[#order + 1] = g end
    table.insert(members[g], e)
  end
  return order, members
end

local function load_idx(i)
  local pl = read_playlists()[i]
  if not pl then return nil, "Playlist nicht gefunden" end
  return m3u.load(pl.url)
end

return {
  name = "M3U-Playlists & Favoriten",
  description = "Eigene Listen, Favoriten und IPTV-Listen aus playlists.txt",

  browse = function(id)
    if id == nil then
      local items = {}
      for i, pl in ipairs(read_playlists()) do
        local local_file = pl.url:match("^file:")
        items[#items + 1] = { title = pl.name, kind = "folder", id = "pl:" .. i .. ":0",
                              subtitle = local_file and "Eigene Playlist" or pl.url }
      end
      if #items == 0 then return nil, "Noch keine Playlists - mit Quadrat Eintraege speichern" end
      return items
    end

    -- pl:<nr>:<offset>  oder  grp:<nr>:<gruppe>:<offset>
    local pidx, off = id:match("^pl:(%d+):(%d+)$")
    if pidx then
      local entries, err = load_idx(tonumber(pidx))
      if not entries then return nil, err end
      if #entries == 0 then return nil, "Playlist ist leer" end
      local order, members = groups_of(entries)
      if #order > 1 and #entries > 40 then
        local items = {}
        for gi, g in ipairs(order) do
          items[#items + 1] = { title = g, subtitle = #members[g] .. " Eintraege", kind = "folder",
                                id = "grp:" .. pidx .. ":" .. gi .. ":0",
                                thumb = members[g][1].logo }
        end
        return items
      end
      return m3u.page(entries, tonumber(off), PAGE, function(n) return "pl:" .. pidx .. ":" .. n end)
    end

    local gp, gi, goff = id:match("^grp:(%d+):(%d+):(%d+)$")
    if gp then
      local entries, err = load_idx(tonumber(gp))
      if not entries then return nil, err end
      local order, members = groups_of(entries)
      local list = members[order[tonumber(gi)]] or {}
      return m3u.page(list, tonumber(goff), PAGE, function(n) return "grp:" .. gp .. ":" .. gi .. ":" .. n end)
    end
    return nil, "Unbekannter Eintrag"
  end,

  -- Durchsucht alle Playlists nach Titeln
  search = function(query)
    local q, hits = query:lower(), {}
    for i, pl in ipairs(read_playlists()) do
      local entries = load_idx(i)
      if entries then
        for _, e in ipairs(entries) do
          if e.title:lower():find(q, 1, true) then
            hits[#hits + 1] = e
            if #hits >= 500 then break end
          end
        end
      end
    end
    if #hits == 0 then return nil, "Nichts gefunden fuer: " .. query end
    local items = {}
    for _, e in ipairs(hits) do items[#items + 1] = m3u.item(e) end
    return items
  end,

  resolve = m3u.resolve_item,
}
