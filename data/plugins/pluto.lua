-- Pluto TV auf Abruf (Filme & Serien, kostenlos, werbefinanziert)
--
-- Ablauf wie in der offiziellen Web-App: boot.pluto.tv liefert einen Sitzungs-Token,
-- der VOD-Katalog kommt von api.pluto.tv/v3/vod/..., die Wiedergabe-Adresse baut sich aus
-- dem Stitcher-Server der Sitzung + "/v2" + Pfad des Titels + Sitzungsparametern + jwt.
-- Ohne "drmCapabilities" liefert Pluto die unverschluesselte HLS-Fassung, die die Vita
-- in Hardware dekodiert. Werbung ist serverseitig eingefuegt (wird vom Player als
-- Zeitsprung ueberbrueckt).
--
-- Live-Sender von Pluto gibt es weiterhin im Sender-Finder.

local json = require("json")

local BOOT = "https://boot.pluto.tv/v4/start"
local API = "https://api.pluto.tv"
local IMAGES = "https://images.pluto.tv"
local STITCHER_FALLBACK = "https://cfd-v4-service-channel-stitcher-use1-1.prd.pluto.tv"
local CAT_PAGE = 25      -- Kategorien pro Abruf (jede mit bis zu 100 Titeln)

-- ---------------------------------------------------------------- Hilfen

local function enc(s) return (tostring(s):gsub("[^%w%-_%.~]", function(c) return string.format("%%%02X", c:byte()) end)) end

local function device_id()
  local id = vs.read_file("pluto_device.txt")
  if id and #id >= 32 then return (id:gsub("%s+", "")) end
  local hex = "0123456789abcdef"
  local t = {}
  for i = 1, 32 do local k = math.random(1, 16); t[i] = hex:sub(k, k) end
  id = table.concat(t):gsub("^(%x%x%x%x%x%x%x%x)(%x%x%x%x)(%x%x%x%x)(%x%x%x%x)", "%1-%2-%3-%4-")
  vs.write_file("pluto_device.txt", id)
  return id
end

local function query(params)
  local q = {}
  for _, kv in ipairs(params) do q[#q + 1] = kv[1] .. "=" .. enc(kv[2]) end
  return table.concat(q, "&")
end

local session = nil

local function boot()
  local dev = device_id()
  local params = {
    { "appName", "web" }, { "appVersion", "9.1.0" }, { "deviceType", "web" }, { "deviceModel", "web" },
    { "deviceMake", "chrome" }, { "deviceVersion", "122.0.0" }, { "deviceId", dev }, { "clientID", dev },
    { "clientModelNumber", "1.0.0" }, { "serverSideAds", "false" },
  }
  local body, status = vs.http_get(BOOT .. "?" .. query(params), "Accept: application/json")
  if not body then return nil, "Pluto nicht erreichbar: " .. tostring(status) end
  local ok, j = pcall(json.decode, body)
  if not ok or type(j) ~= "table" or not j.sessionToken then
    return nil, "Pluto: keine Sitzung (HTTP " .. tostring(status) .. ")"
  end
  local sp = j.stitcherParams or ""
  if sp:sub(1, 1) == "?" then sp = sp:sub(2) end
  if sp == "" then sp = query(params) end
  session = { token = j.sessionToken, params = sp,
              stitcher = (j.servers and j.servers.stitcher) or STITCHER_FALLBACK }
  return session
end

local function api(path, params)
  for attempt = 1, 2 do
    if not session then
      local s, err = boot()
      if not s then return nil, err end
    end
    local url = API .. path .. (params and ("?" .. query(params)) or "")
    local body, status = vs.http_get(url, "Accept: application/json\nAuthorization: Bearer " .. session.token)
    if body and status ~= 401 and status ~= 403 then
      if status and status >= 400 then return nil, "Pluto antwortete HTTP " .. status end
      local ok, j = pcall(json.decode, body)
      if not ok then return nil, "Pluto-Antwort unlesbar" end
      return j
    end
    session = nil      -- Token abgelaufen oder Region gewechselt: neu anmelden
    if not body then return nil, "Pluto nicht erreichbar: " .. tostring(status) end
  end
  return nil, "Pluto hat die Sitzung abgelehnt"
end

local function abs_img(p)
  if not p or p == "" then return nil end
  local u = p:match("^https?://") and p or (IMAGES .. (p:sub(1, 1) == "/" and "" or "/") .. p)
  if u:find("images.pluto.tv", 1, true) and not u:find("?", 1, true) then u = u .. "?fm=jpg&w=240" end
  return u
end

local function image_of(o)
  if type(o.covers) == "table" then
    for _, c in ipairs(o.covers) do
      if c.aspectRatio == "16:9" then return abs_img(c.url or c.path) end
    end
  end
  for _, k in ipairs({ "poster16_9", "featuredImage", "tile", "thumbnail", "poster" }) do
    if type(o[k]) == "table" and o[k].path then return abs_img(o[k].path) end
  end
  return nil
end

local function stitched_path(o)
  local st = o.stitched
  if type(st) ~= "table" then return nil end
  for _, l in ipairs(st.paths or st.urls or {}) do
    local p = l.path or l.url
    if p and (not l.type or l.type == "hls") then return p end
  end
  return st.path
end

local function fmt_dur(ms)
  ms = tonumber(ms)
  if not ms or ms <= 0 then return nil end
  local m = math.floor(ms / 60000)
  if m >= 60 then return string.format("%d Std. %02d Min.", m // 60, m % 60) end
  return m .. " Min."
end

local function is_series(o) return o.type == "series" or (type(o.seasonsNumbers) == "table" and #o.seasonsNumbers > 0) end

local function item_of(o)
  local id = o._id or o.id
  if not id then return nil end
  if is_series(o) then
    local n = type(o.seasonsNumbers) == "table" and #o.seasonsNumbers or 0
    local sub = {}
    if o.genre then sub[#sub + 1] = o.genre end
    if n > 0 then sub[#sub + 1] = n .. (n == 1 and " Staffel" or " Staffeln") end
    return { title = o.name or "?", subtitle = table.concat(sub, "  |  "), id = "series:" .. id,
             kind = "folder", thumb = image_of(o), genre = o.genre }
  end
  local sub = {}
  if o.genre then sub[#sub + 1] = o.genre end
  local d = fmt_dur(o.duration)
  if d then sub[#sub + 1] = d end
  if o.rating and o.rating ~= "" then sub[#sub + 1] = "FSK/Rating " .. o.rating end
  return { title = o.name or "?", subtitle = table.concat(sub, "  |  "), id = "ep:" .. id, kind = "video",
           thumb = image_of(o), spath = stitched_path(o), genre = o.genre }
end

-- ---------------------------------------------------------------- Katalog (seitenweise)

local pages = {}   -- Seite -> { {name, items = {...}, total}, ... }

local function load_page(page)
  if pages[page] then return pages[page] end
  vs.log("Lade Pluto-Katalog (Seite " .. (page + 1) .. ") ...")
  local j, err = api("/v3/vod/categories", {
    { "includeItems", "true" }, { "deviceType", "web" }, { "offset", tostring(CAT_PAGE) }, { "page", tostring(page + 1) },
  })
  if not j then return nil, err end
  local cats = {}
  for _, c in ipairs(j.categories or {}) do
    local list = {}
    for _, o in ipairs(c.items or {}) do
      local it = item_of(o)
      if it then list[#list + 1] = it end
    end
    if #list > 0 then
      cats[#cats + 1] = { name = c.name or "?", items = list, total = c.totalItemsCount }
    end
  end
  pages[page] = cats
  return cats
end

local function category_folders(page)
  local cats, err = load_page(page)
  if not cats then return nil, err end
  if #cats == 0 then return nil, page == 0 and "Pluto lieferte keine Inhalte (Region?)" or "Keine weiteren Kategorien" end
  local items = {}
  for i, c in ipairs(cats) do
    local n = #c.items
    items[#items + 1] = { title = c.name, kind = "folder", id = "cat:" .. page .. ":" .. i,
                          subtitle = ((c.total and c.total > n) and (n .. "+") or tostring(n)) .. " Titel",
                          thumb = c.items[1] and c.items[1].thumb }
  end
  if #cats >= CAT_PAGE then
    items[#items + 1] = { title = "Weitere Kategorien laden", kind = "more", id = "catpage:" .. (page + 1) }
  end
  return items
end

-- Filme oder Serien aus allen geladenen Kategorien, nach Genre gruppiert
local function by_genre(want_series)
  local cats, err = load_page(0)
  if not cats then return nil, err end
  local seen, genres, order = {}, {}, {}
  for _, c in ipairs(cats) do
    for _, it in ipairs(c.items) do
      local s = it.kind == "folder"
      if s == want_series and not seen[it.id] then
        seen[it.id] = true
        local g = (it.genre and it.genre ~= "") and it.genre or "Sonstiges"
        if not genres[g] then genres[g] = {}; order[#order + 1] = g end
        table.insert(genres[g], it)
      end
    end
  end
  table.sort(order)
  return genres, order
end

local genre_cache = {}

-- ---------------------------------------------------------------- Quelle

return {
  name = "Pluto TV (auf Abruf)",
  description = "Kostenlose Filme & Serien, werbefinanziert - Live-Sender im Sender-Finder",

  browse = function(id)
    if id == nil then
      return {
        { title = "Filme", subtitle = "nach Genre", id = "genres:movies", kind = "folder" },
        { title = "Serien", subtitle = "nach Genre - Staffeln und Folgen in Reihenfolge", id = "genres:series", kind = "folder" },
        { title = "Alle Kategorien", subtitle = "wie auf pluto.tv", id = "catpage:0", kind = "folder" },
      }
    end

    local kind = id:match("^genres:(%a+)$")
    if kind then
      local genres, order = by_genre(kind == "series")
      if not genres then return nil, order end
      genre_cache[kind] = genres
      local items = {}
      for _, g in ipairs(order) do
        items[#items + 1] = { title = g, subtitle = #genres[g] .. " Titel", id = "genre:" .. kind .. ":" .. g,
                              kind = "folder", thumb = genres[g][1].thumb }
      end
      if #items == 0 then return nil, "Keine Titel gefunden" end
      return items
    end
    local gk, gname = id:match("^genre:(%a+):(.+)$")
    if gk then
      if not genre_cache[gk] then genre_cache[gk] = by_genre(gk == "series") end
      local list = genre_cache[gk] and genre_cache[gk][gname]
      if not list then return nil, "Bitte erneut oeffnen" end
      return list
    end

    local cp = id:match("^catpage:(%d+)$")
    if cp then return category_folders(tonumber(cp)) end
    local pg, ci = id:match("^cat:(%d+):(%d+)$")
    if pg then
      local cats, err = load_page(tonumber(pg))
      if not cats then return nil, err end
      local c = cats[tonumber(ci)]
      if not c then return nil, "Kategorie nicht mehr vorhanden" end
      return c.items
    end

    -- Serie: Staffeln (eine Anfrage liefert alle Folgen aller Staffeln)
    local sid, snum = id:match("^series:([^:]+):?(%d*)$")
    if sid then
      local j, err = api("/v3/vod/series/" .. enc(sid) .. "/seasons", { { "includeItems", "true" }, { "deviceType", "web" } })
      if not j then return nil, err end
      local seasons = j.seasons or {}
      table.sort(seasons, function(a, b) return (tonumber(a.number) or 0) < (tonumber(b.number) or 0) end)
      if snum == "" then
        if #seasons == 1 then snum = tostring(seasons[1].number) else
          local items = {}
          for _, s in ipairs(seasons) do
            items[#items + 1] = { title = "Staffel " .. tostring(s.number), kind = "folder",
                                  subtitle = #(s.episodes or {}) .. " Folgen", id = "series:" .. sid .. ":" .. tostring(s.number),
                                  thumb = image_of(j) }
          end
          if #items == 0 then return nil, "Keine Folgen verfuegbar" end
          return items
        end
      end
      for _, s in ipairs(seasons) do
        if tostring(s.number) == snum then
          local eps = s.episodes or {}
          table.sort(eps, function(a, b) return (tonumber(a.number) or 0) < (tonumber(b.number) or 0) end)
          local items = {}
          for _, e in ipairs(eps) do
            local it = item_of(e)
            if it then
              if e.number then it.title = e.number .. ". " .. it.title end
              items[#items + 1] = it
            end
          end
          if #items == 0 then return nil, "Keine Folgen verfuegbar" end
          return items
        end
      end
      return nil, "Staffel nicht gefunden"
    end
    return nil, "Unbekannter Eintrag"
  end,

  resolve = function(item)
    local id = item.id:match("^ep:(.+)$")
    if not id then return nil, "Kein abspielbarer Titel" end
    -- frische Sitzung: der Token steckt in der Adresse und laeuft ab
    session = nil
    local s, err = boot()
    if not s then return nil, err end
    local p = item.spath or ("/stitch/hls/episode/" .. id .. "/master.m3u8")
    p = p:gsub("^https?://[^/]+", ""):gsub("%?.*$", "")
    if p:sub(1, 1) ~= "/" then p = "/" .. p end
    p = p:gsub("^/v%d+/", "/")
    return s.stitcher .. "/v2" .. p .. "?" .. s.params .. "&jwt=" .. enc(s.token) ..
           "&masterJWTPassthrough=true&includeExtendedEvents=true"
  end,
}
