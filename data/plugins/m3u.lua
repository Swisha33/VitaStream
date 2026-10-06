-- M3U-Playlists (eigene Listen, IPTV, Favoriten) - mit Bearbeiten ueber die Quadrat-Taste.
-- Die Playlists stehen in ux0:data/VitaStream/playlists.txt, eine pro Zeile:
--     Name|https://example.org/liste.m3u
--     Lokale Liste|file:meine_liste.m3u      (Datei in ux0:data/VitaStream/)
-- Eigene (lokale) Listen lassen sich bearbeiten; von Online-Listen kann man eine
-- bearbeitbare Kopie anlegen.

local m3u = require("m3ulib")
local PAGE = 100

-- ---------------------------------------------------------------- playlists.txt

local function read_lines()
  local lines = {}
  local txt = vs.read_file("playlists.txt") or ""
  for line in txt:gmatch("[^\r\n]*") do lines[#lines + 1] = line end
  while #lines > 0 and lines[#lines] == "" do lines[#lines] = nil end
  return lines
end

local function read_playlists()
  local list = {}
  for ln, line in ipairs(read_lines()) do
    if not line:match("^%s*#") then
      local name, url = line:match("^%s*(.-)%s*|%s*(%S+)%s*$")
      if name and url and #name > 0 then list[#list + 1] = { name = name, url = url, line = ln } end
    end
  end
  return list
end

local function write_lines(lines)
  vs.write_file("playlists.txt", table.concat(lines, "\n") .. "\n")
end

local function file_of(pl) return pl.url:match("^file:(.+)$") end

local function safe_filename(name)
  local f = name:lower():gsub("[^%w]+", "_"):gsub("^_+", ""):gsub("_+$", "")
  if f == "" then f = "playlist" end
  return f .. ".m3u"
end

-- ---------------------------------------------------------------- Laden

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
  return m3u.load(pl.url), pl
end

local function entry_items(list, pidx, offset, more_fn)
  local items = m3u.page(list, offset, PAGE, more_fn)
  for _, it in ipairs(items) do if it.kind == "video" then it.pl = pidx end end
  return items
end

-- ---------------------------------------------------------------- Aktionen

local function save_entries(pl, entries)
  local f = file_of(pl)
  if not f then return nil, "Online-Liste - bitte zuerst eine bearbeitbare Kopie anlegen" end
  m3u.save(f, entries)
  return true
end

-- Online-Liste als lokale Datei speichern und in playlists.txt eintragen
local function make_copy(pl, entries, name)
  local file = safe_filename(name)
  m3u.save(file, entries)
  local lines = read_lines()
  lines[#lines + 1] = name .. "|file:" .. file
  write_lines(lines)
  return file
end

local function actions(item)
  local acts = {}
  local pls = read_playlists()
  local pl = item.pl and pls[item.pl]
  if not pl then return acts end
  local editable = file_of(pl) ~= nil

  if item.kind == "video" then
    acts[#acts + 1] = { id = "check", label = "Stream pruefen" }
    if editable then
      acts[#acts + 1] = { id = "rename", label = "Umbenennen", input = "Neuer Name", default = item.title }
      acts[#acts + 1] = { id = "up", label = "Nach oben verschieben" }
      acts[#acts + 1] = { id = "down", label = "Nach unten verschieben" }
      acts[#acts + 1] = { id = "delete", label = "Eintrag loeschen", confirm = true }
    end
  elseif item.group then
    if editable then
      acts[#acts + 1] = { id = "group_check", label = "Defekte Streams der Gruppe entfernen (dauert)" }
      acts[#acts + 1] = { id = "group_delete", label = "Ganze Gruppe loeschen", confirm = true }
    end
  else  -- Playlist auf der Startseite
    if editable then
      acts[#acts + 1] = { id = "pl_check", label = "Defekte Streams entfernen (dauert)" }
      acts[#acts + 1] = { id = "pl_rename", label = "Playlist umbenennen", input = "Neuer Name", default = pl.name }
      acts[#acts + 1] = { id = "pl_delete", label = "Playlist loeschen", confirm = true }
    else
      acts[#acts + 1] = { id = "pl_copy", label = "Bearbeitbare Kopie anlegen", input = "Name der Kopie", default = pl.name .. " (eigene)" }
      acts[#acts + 1] = { id = "pl_copy_check", label = "Kopie nur mit funktionierenden Streams (dauert)", input = "Name der Kopie", default = pl.name .. " (geprueft)" }
      acts[#acts + 1] = { id = "pl_remove", label = "Aus der Liste entfernen", confirm = true }
    end
  end
  return acts
end

local function action(item, id, input)
  local pls = read_playlists()
  local pl = item.pl and pls[item.pl]
  if not pl then return nil, "Playlist nicht gefunden" end
  local entries, err = m3u.load(pl.url)

  -- ---- einzelner Eintrag
  if id == "check" then
    local ok, info = vs.probe(item.url, m3u.header_text(item.headers))
    return { message = (ok and "Stream antwortet: " or "Stream defekt: ") .. info }
  end
  if not entries then return nil, err end
  local pos
  for i, e in ipairs(entries) do if e.idx == item.idx then pos = i break end end

  if id == "delete" or id == "rename" or id == "up" or id == "down" then
    if not pos then return nil, "Eintrag nicht mehr vorhanden" end
    if id == "delete" then table.remove(entries, pos)
    elseif id == "rename" then entries[pos].title = input
    elseif id == "up" and pos > 1 then entries[pos], entries[pos - 1] = entries[pos - 1], entries[pos]
    elseif id == "down" and pos < #entries then entries[pos], entries[pos + 1] = entries[pos + 1], entries[pos] end
    local ok, e2 = save_entries(pl, entries)
    if not ok then return nil, e2 end
    return { refresh = true }
  end

  -- ---- Gruppe
  if id == "group_delete" or id == "group_check" then
    local keep, members = {}, {}
    for _, e in ipairs(entries) do
      local g = (e.group and #e.group > 0) and e.group or "Allgemein"
      if g == item.group then members[#members + 1] = e else keep[#keep + 1] = e end
    end
    local msg
    if id == "group_check" then
      local alive, dead = m3u.check_all(members)
      for _, e in ipairs(alive) do keep[#keep + 1] = e end
      msg = string.format("%d von %d Streams defekt und entfernt.", dead, #members)
    else
      msg = string.format("Gruppe \"%s\" mit %d Eintraegen geloescht.", item.group, #members)
    end
    local ok, e2 = save_entries(pl, keep)
    if not ok then return nil, e2 end
    return { message = msg, refresh = true }
  end

  -- ---- ganze Playlist
  if id == "pl_check" then
    local alive, dead = m3u.check_all(entries)
    local ok, e2 = save_entries(pl, alive)
    if not ok then return nil, e2 end
    return { message = string.format("%d von %d Streams defekt und entfernt.", dead, #entries), refresh = true }
  end
  if id == "pl_copy" or id == "pl_copy_check" then
    local list, msg = entries, ""
    if id == "pl_copy_check" then
      local dead
      list, dead = m3u.check_all(entries)
      msg = string.format(" %d defekte Streams wurden weggelassen.", dead)
    end
    make_copy(pl, list, input)
    return { message = "Kopie \"" .. input .. "\" angelegt (" .. #list .. " Eintraege)." .. msg, refresh = true }
  end
  if id == "pl_rename" or id == "pl_delete" or id == "pl_remove" then
    local lines = read_lines()
    if id == "pl_rename" then
      lines[pl.line] = input .. "|" .. pl.url
    else
      table.remove(lines, pl.line)
      if id == "pl_delete" and file_of(pl) then vs.delete_file(file_of(pl)) end
    end
    write_lines(lines)
    return { refresh = true }
  end
  return nil, "Unbekannte Aktion"
end

-- ---------------------------------------------------------------- Quelle

return {
  name = "M3U-Playlists & Favoriten",
  description = "Eigene Listen, Favoriten, IPTV-Listen - Quadrat: bearbeiten",

  browse = function(id)
    if id == nil then
      local items = {}
      for i, pl in ipairs(read_playlists()) do
        local local_file = file_of(pl)
        items[#items + 1] = { title = pl.name, kind = "folder", id = "pl:" .. i .. ":0", pl = i,
                              subtitle = local_file and "Eigene Playlist (bearbeitbar)" or pl.url }
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
                                id = "grp:" .. pidx .. ":" .. gi .. ":0", pl = tonumber(pidx), group = g,
                                thumb = members[g][1].logo }
        end
        return items
      end
      return entry_items(entries, tonumber(pidx), tonumber(off), function(n) return "pl:" .. pidx .. ":" .. n end)
    end

    local gp, gi, goff = id:match("^grp:(%d+):(%d+):(%d+)$")
    if gp then
      local entries, err = load_idx(tonumber(gp))
      if not entries then return nil, err end
      local order, members = groups_of(entries)
      local list = members[order[tonumber(gi)]]
      if not list or #list == 0 then return nil, "Gruppe ist leer" end
      return entry_items(list, tonumber(gp), tonumber(goff), function(n) return "grp:" .. gp .. ":" .. gi .. ":" .. n end)
    end
    return nil, "Unbekannter Eintrag"
  end,

  -- Durchsucht alle Playlists nach Titeln
  search = function(query)
    local q, items = query:lower(), {}
    for i, pl in ipairs(read_playlists()) do
      local entries = m3u.load(pl.url)
      if entries then
        for _, e in ipairs(entries) do
          if e.title:lower():find(q, 1, true) then
            local it = m3u.item(e, pl.name)
            it.pl = i
            items[#items + 1] = it
            if #items >= 500 then break end
          end
        end
      end
    end
    if #items == 0 then return nil, "Nichts gefunden fuer: " .. query end
    return items
  end,

  actions = actions,
  action = action,
  resolve = m3u.resolve_item,
  info = m3u.info,
}
