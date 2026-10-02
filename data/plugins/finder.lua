-- Sender-Finder: frei empfangbare TV-Sender aus dem iptv-org-Verzeichnis
-- (https://github.com/iptv-org/iptv) nach Kategorie, Land und Sprache, mit Suche.
-- Mit der Quadrat-Taste lassen sich Sender in Favoriten oder eigene Playlists uebernehmen.

local m3u = require("m3ulib")
local json = require("json")
local BASE = "https://iptv-org.github.io/iptv/"
local API = "https://iptv-org.github.io/api/"
local PAGE = 60

local CAT_DE = {
  animation = "Zeichentrick & Anime", auto = "Auto", business = "Wirtschaft", classic = "Klassiker",
  comedy = "Comedy", cooking = "Kochen", culture = "Kultur", documentary = "Dokumentationen",
  education = "Bildung", entertainment = "Unterhaltung", family = "Familie", general = "Allgemein",
  kids = "Kinder", legislative = "Politik", lifestyle = "Lifestyle", movies = "Filme", music = "Musik",
  news = "Nachrichten", outdoor = "Outdoor", relax = "Entspannung", religious = "Religion",
  series = "Serien", science = "Wissenschaft", shop = "Shopping", sports = "Sport", travel = "Reisen",
  weather = "Wetter",
}
local HIDDEN = { xxx = true }

local LANGS = {
  { "deu", "Deutsch" }, { "eng", "Englisch" }, { "tur", "Tuerkisch" }, { "fra", "Franzoesisch" },
  { "spa", "Spanisch" }, { "ita", "Italienisch" }, { "pol", "Polnisch" }, { "rus", "Russisch" },
  { "ara", "Arabisch" }, { "nld", "Niederlaendisch" }, { "por", "Portugiesisch" }, { "jpn", "Japanisch" },
  { "kor", "Koreanisch" }, { "hrv", "Kroatisch" }, { "srp", "Serbisch" }, { "ell", "Griechisch" },
}

-- Kostenlose (werbefinanzierte) Dienste, die in den Listen enthalten sind
local SERVICES = {
  { "Pluto TV", "pluto" }, { "Samsung TV Plus", "samsung" }, { "Rakuten TV", "rakuten" },
  { "Plex", "plex" }, { "Zattoo/Waipu Free", "waipu" },
}

local function folder(title, id, sub) return { title = title, id = id, kind = "folder", subtitle = sub } end

-- Liste laden und optional filtern (Text in Titel oder URL)
local function list_for(path, filter)
  local entries, err = m3u.load(BASE .. path)
  if not entries then return nil, err end
  if not filter or filter == "" then return entries end
  local out, f = {}, filter:lower()
  for _, e in ipairs(entries) do
    if e.title:lower():find(f, 1, true) or e.url:lower():find(f, 1, true) then out[#out + 1] = e end
  end
  return out
end

local function show(path, filter, offset)
  local list, err = list_for(path, filter)
  if not list then return nil, err end
  if #list == 0 then return nil, "Keine Sender gefunden" end
  return m3u.page(list, offset, PAGE, function(n)
    return "list:" .. path .. "|" .. (filter or "") .. "|" .. n
  end)
end

return {
  name = "Sender-Finder (frei empfangbar)",
  description = "Tausende freie TV-Sender: Kategorien, Laender, Sprachen, Pluto TV & Co.",

  browse = function(id)
    if id == nil then
      return {
        folder("Zeichentrick & Anime", "list:categories/animation.m3u||0", "Animationssender weltweit"),
        folder("Deutschland", "list:countries/de.m3u||0", "Alle frei empfangbaren Sender aus DE"),
        folder("Oesterreich", "list:countries/at.m3u||0"),
        folder("Schweiz", "list:countries/ch.m3u||0"),
        folder("Kostenlose Dienste", "services", "Pluto TV, Samsung TV Plus, Rakuten TV ..."),
        folder("Kategorien", "cats", "Filme, Serien, Sport, Nachrichten ..."),
        folder("Laender", "countries"),
        folder("Sprachen", "langs"),
      }
    end

    if id == "services" then
      local items = {}
      for _, s in ipairs(SERVICES) do
        items[#items + 1] = folder(s[1] .. " (Deutschland)", "list:countries/de.m3u|" .. s[2] .. "|0")
        items[#items + 1] = folder(s[1] .. " (deutschsprachig)", "list:languages/deu.m3u|" .. s[2] .. "|0")
      end
      return items
    end

    if id == "cats" then
      local text = vs.http_get(API .. "categories.json")
      local cats = text and json.decode(text)
      local items = {}
      if type(cats) == "table" then
        for _, c in ipairs(cats) do
          if not HIDDEN[c.id] then
            items[#items + 1] = folder(CAT_DE[c.id] or c.name, "list:categories/" .. c.id .. ".m3u||0")
          end
        end
        table.sort(items, function(a, b) return a.title < b.title end)
      end
      if #items == 0 then
        for cid, name in pairs(CAT_DE) do items[#items + 1] = folder(name, "list:categories/" .. cid .. ".m3u||0") end
        table.sort(items, function(a, b) return a.title < b.title end)
      end
      return items
    end

    if id == "countries" then
      vs.log("Lade Laenderliste ...")
      local text, err = vs.http_get(API .. "countries.json")
      local list = text and json.decode(text)
      if type(list) ~= "table" then return nil, err or "Laenderliste nicht lesbar" end
      local items = {}
      for _, c in ipairs(list) do
        if c.code then
          items[#items + 1] = folder(c.name or c.code, "list:countries/" .. c.code:lower() .. ".m3u||0", c.code)
        end
      end
      table.sort(items, function(a, b) return a.title < b.title end)
      return items
    end

    if id == "langs" then
      local items = {}
      for _, l in ipairs(LANGS) do items[#items + 1] = folder(l[2], "list:languages/" .. l[1] .. ".m3u||0") end
      return items
    end

    local path, filter, off = id:match("^list:([^|]+)|([^|]*)|(%d+)$")
    if path then return show(path, filter, tonumber(off)) end
    return nil, "Unbekannter Eintrag"
  end,

  -- Suche ueber alle Sender (Name)
  search = function(query)
    vs.log("Lade Gesamtverzeichnis (einmalig, dauert etwas) ...")
    local entries, err = m3u.load(BASE .. "index.m3u")
    if not entries then return nil, err end
    local q, hits = query:lower(), {}
    for _, e in ipairs(entries) do
      if e.title:lower():find(q, 1, true) or (e.group or ""):lower():find(q, 1, true) then
        hits[#hits + 1] = e
        if #hits >= 600 then break end
      end
    end
    if #hits == 0 then return nil, "Kein Sender gefunden fuer: " .. query end
    local items = {}
    for _, e in ipairs(hits) do items[#items + 1] = m3u.item(e) end
    return items
  end,

  resolve = m3u.resolve_item,
}
