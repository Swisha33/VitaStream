-- Direkte URL: beliebige MP4- oder HLS-Adresse (oder Datei auf ux0:) abspielen.
-- Merkt sich die letzten Adressen in history_direct.txt.

local HISTORY = "history_direct.txt"

local function load_history()
  local list = {}
  local txt = vs.read_file(HISTORY)
  if txt then
    for line in txt:gmatch("[^\r\n]+") do list[#list + 1] = line end
  end
  return list
end

local function item_for(url)
  local kind = url:match("%.m3u8") and "HLS" or (url:match("^ux0:") and "Lokale Datei" or "MP4/Direkt")
  return { title = url, subtitle = kind, id = url, kind = "video" }
end

return {
  name = "Direkte URL",
  description = "MP4-/HLS-Link oder Datei (ux0:...) direkt abspielen. Dreieck: neue URL",

  browse = function(_)
    local items = {}
    for _, url in ipairs(load_history()) do items[#items + 1] = item_for(url) end
    if #items == 0 then
      vs.log("Noch keine URLs - mit Dreieck eine eingeben")
    end
    return items
  end,

  search = function(query)
    local url = query:gsub("^%s+", ""):gsub("%s+$", "")
    if not (url:match("^https?://") or url:match("^ux0:")) then
      return nil, "Bitte eine URL mit http(s):// oder einen Pfad mit ux0: eingeben"
    end
    return { item_for(url) }
  end,

  resolve = function(item)
    -- in den Verlauf aufnehmen (neueste zuerst, max. 30)
    local list, out = load_history(), { item.id }
    for _, u in ipairs(list) do
      if u ~= item.id and #out < 30 then out[#out + 1] = u end
    end
    vs.write_file(HISTORY, table.concat(out, "\n") .. "\n")
    return item.id
  end,
}
