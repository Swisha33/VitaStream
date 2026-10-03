-- Downloads: heruntergeladene Videos und Podcasts offline abspielen
-- (Dateien in ux0:data/VitaStream/downloads/ - auch eigene Dateien per FTP dort ablegen)

local AUDIO = { mp3 = true, m4a = true, aac = true }

local function fmt_size(n)
  n = tonumber(n) or 0
  if n >= 1073741824 then return string.format("%.2f GB", n / 1073741824) end
  return string.format("%.0f MB", n / 1048576)
end

return {
  name = "Downloads",
  description = "Heruntergeladene Videos und Podcasts - offline abspielen",

  browse = function(id)
    if id ~= nil then return nil, "Unbekannter Eintrag" end
    local files = vs.list_downloads()
    table.sort(files, function(a, b) return a.name:lower() < b.name:lower() end)
    local items = {}
    for _, f in ipairs(files) do
      local ext = (f.name:match("%.(%w+)$") or ""):lower()
      items[#items + 1] = {
        title = f.name:gsub("%.%w+$", ""):gsub("_", " "),
        subtitle = (AUDIO[ext] and "Audio" or "Video") .. "  |  " .. fmt_size(f.size) .. "  |  " .. ext:upper(),
        id = f.path, url = f.path, kind = "video", file = f.name,
      }
    end
    if #items == 0 then
      return nil, "Noch keine Downloads. In einer Liste mit Quadrat -> \"Herunterladen\" waehlen."
    end
    return items
  end,

  actions = function(item)
    if item.file then return { { id = "del", label = "Datei loeschen", confirm = true } } end
    return {}
  end,

  action = function(item, id)
    if id == "del" and item.file then
      if vs.delete_download(item.file) then return { message = "Geloescht: " .. item.file, refresh = true } end
      return nil, "Loeschen fehlgeschlagen"
    end
    return nil, "Unbekannte Aktion"
  end,

  resolve = function(item) return item.url or item.id end,
}
