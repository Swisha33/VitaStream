-- South Park: kostenlose Folgen von southpark.de (offizielle Seite, Deutsch & Englisch).
-- Ablauf wie bei yt-dlp: Seitendaten als JSON (?json=true) -> videoServiceUrl -> HLS-Stream.
-- Die Seite liefert Folgen nur in bestimmten Laendern (DE/AT/CH); sonst leitet sie um.

local json = require("json")
local HOST = "https://www.southpark.de"

local function abs(u)
  if not u then return nil end
  if u:match("^https?://") then return u end
  if u:sub(1, 2) == "//" then return "https:" .. u end
  if u:sub(1, 1) == "/" then return HOST .. u end
  return HOST .. "/" .. u
end

local function with_json(u)
  return u .. (u:find("?", 1, true) and "&" or "?") .. "json=true"
end

local function get_json(url)
  local body, status, final = vs.http_get(with_json(url), "Accept: application/json")
  if not body then return nil, status end
  if status and status >= 400 then return nil, "HTTP " .. status end
  if final and not final:find("southpark%.de") then
    return nil, "South Park ist von hier aus nicht verfuegbar (Weiterleitung nach " .. final:match("//([^/]+)") .. ")"
  end
  local data = json.decode(body)
  if type(data) ~= "table" then return nil, "Seitendaten nicht lesbar" end
  return data
end

-- Durchlaeuft alle Tabellen des JSON-Baums
local function walk(t, fn, depth)
  depth = depth or 0
  if type(t) ~= "table" or depth > 40 then return end
  fn(t)
  for _, v in pairs(t) do
    if type(v) == "table" then walk(v, fn, depth + 1) end
  end
end

local function first_image(t)
  local found
  walk(t, function(x)
    if found then return end
    for _, v in pairs(x) do
      if type(v) == "string" and (v:match("^https://images%.paramount%.tech") or v:match("mtvnimages%.com")) then
        found = v
        return
      end
    end
  end)
  if found and found:match("images%.paramount%.tech") and not found:find("?", 1, true) then
    found = found .. "?width=224&quality=0.7"
  end
  return found
end

local function text_of(t, keys)
  for _, k in ipairs(keys) do
    local v = t[k]
    if type(v) == "string" and #v > 0 then return v end
    if type(v) == "table" then
      for _, k2 in ipairs({ "title", "text", "label", "value" }) do
        if type(v[k2]) == "string" and #v[k2] > 0 then return v[k2] end
      end
    end
  end
end

local function is_episode(u) return u and (u:match("/folgen/%w+/") or u:match("/episodes/%w+/")) end
local function is_season(u) return u and u:match("/seasons/south%-park/%w+/") end

local function collect(data)
  local seasons, episodes, seen = {}, {}, {}
  local more
  walk(data, function(t)
    local u = t.url or t.href or t.link
    if type(u) == "table" then u = u.url or u.href end
    if type(u) ~= "string" then
      if type(t.loadMore) == "table" and type(t.loadMore.url) == "string" then more = t.loadMore.url end
      return
    end
    if is_episode(u) and not seen[u] then
      seen[u] = true
      local meta = t.meta
      local header = type(meta) == "table" and type(meta.header) == "table" and meta.header or nil
      local title = text_of(t, { "title", "label", "text", "name" }) or (header and text_of(header, { "title" }))
                    or u:match("/([^/]+)$"):gsub("%-", " ")
      local sub = type(meta) == "table" and (text_of(meta, { "subHeader", "description", "date" })) or text_of(t, { "description", "subTitle" })
      episodes[#episodes + 1] = { title = title, subtitle = sub, id = abs(u), kind = "video",
                                  thumb = first_image(t) or ("og:" .. abs(u)) }
    elseif is_season(u) and not seen[u] then
      seen[u] = true
      local title = text_of(t, { "label", "title", "text", "name" }) or u:match("/([^/]+)$"):gsub("%-", " ")
      seasons[#seasons + 1] = { title = title, id = "season:" .. abs(u), kind = "folder" }
    end
    if type(t.loadMore) == "table" and type(t.loadMore.url) == "string" then more = t.loadMore.url end
  end)
  return seasons, episodes, more
end

-- Fallback: Links direkt aus dem HTML
local function collect_html(url)
  local html = vs.http_get(url)
  if not html then return {}, {} end
  local seasons, episodes, seen = {}, {}, {}
  for u in html:gmatch('href="(/seasons/south%-park/%w+/[%w%-]+)"') do
    if not seen[u] then seen[u] = true; seasons[#seasons + 1] = { title = u:match("/([^/]+)$"):gsub("%-", " "), id = "season:" .. abs(u), kind = "folder" } end
  end
  for u in html:gmatch('href="([^"]-/folgen/%w+/[%w%-]+)"') do
    if not seen[u] then seen[u] = true; episodes[#episodes + 1] = { title = u:match("/([^/]+)$"):gsub("%-", " "), id = abs(u), kind = "video", thumb = "og:" .. abs(u) } end
  end
  return seasons, episodes
end

local function season_sort(list)
  table.sort(list, function(a, b)
    local na, nb = tonumber(a.title:match("(%d+)")), tonumber(b.title:match("(%d+)"))
    if na and nb then return na < nb end
    return a.title < b.title
  end)
end

local function list_page(url)
  local data, err = get_json(url)
  local seasons, episodes, more
  if data then seasons, episodes, more = collect(data)
  else
    if err and err:find("nicht verfuegbar") then return nil, err end
    seasons, episodes = collect_html(url)
  end
  return seasons, episodes, more, err
end

return {
  name = "South Park",
  description = "Alle Staffeln kostenlos von southpark.de (Deutsch/Englisch)",

  browse = function(id)
    if id == nil then
      return {
        { title = "Staffeln (Deutsch)", id = "root:" .. HOST .. "/seasons/south-park", kind = "folder" },
        { title = "Seasons (English)", id = "root:" .. HOST .. "/en/seasons/south-park", kind = "folder" },
      }
    end

    local root = id:match("^root:(.+)$")
    if root then
      vs.log("Lade Staffeln ...")
      local seasons, episodes, _, err = list_page(root)
      if not seasons then return nil, err end
      season_sort(seasons)
      -- die Startseite zeigt meist schon Folgen der aktuellen Staffel
      local items = {}
      for _, s in ipairs(seasons) do items[#items + 1] = s end
      if #items == 0 then
        if #episodes > 0 then return episodes end
        return nil, err or "Keine Staffeln gefunden (Seitenaufbau geaendert?)"
      end
      return items
    end

    local season = id:match("^season:(.+)$")
    if season then
      vs.log("Lade Folgen ...")
      local _, episodes, more, err = list_page(season)
      if not episodes then return nil, err end
      if #episodes == 0 then return nil, err or "Keine Folgen gefunden" end
      if more then episodes[#episodes + 1] = { title = "Weitere Folgen laden", kind = "more", id = "more:" .. abs(more) } end
      return episodes
    end

    local more = id:match("^more:(.+)$")
    if more then
      local data, err = get_json(more)
      if not data then return nil, err end
      local _, episodes, next_more = collect(data)
      if next_more and abs(next_more) ~= more then
        episodes[#episodes + 1] = { title = "Weitere Folgen laden", kind = "more", id = "more:" .. abs(next_more) }
      end
      return episodes
    end
    return nil, "Unbekannter Eintrag"
  end,

  resolve = function(item)
    local page = item.id
    vs.log("Lade Folge ...")
    local service
    local data = get_json(page)
    if data then
      walk(data, function(t)
        if not service and type(t.videoServiceUrl) == "string" then service = t.videoServiceUrl end
      end)
    end
    if not service then
      local html = vs.http_get(page)
      service = html and html:match('"videoServiceUrl"%s*:%s*"([^"]+)"')
      if service then service = service:gsub("\\u002F", "/"):gsub("\\/", "/") end
    end
    if not service then return nil, "Keine Video-Adresse gefunden (Folge evtl. nicht frei verfuegbar)" end
    service = service:gsub("%?.*$", "")
    local body, status = vs.http_get(service .. "?clientPlatform=desktop", "Accept: application/json")
    if not body then return nil, status end
    local info = json.decode(body)
    local st = type(info) == "table" and info.stitchedstream
    if type(st) ~= "table" or not st.source then return nil, "Stream-Daten unvollstaendig" end
    if st.manifesttype and st.manifesttype ~= "hls" then
      return nil, "Stream-Format " .. tostring(st.manifesttype) .. " wird nicht unterstuetzt"
    end
    return { url = st.source, headers = { Referer = HOST .. "/" } }
  end,
}
