-- Oeffentlich-rechtliche Mediatheken (ARD, ZDF, arte, 3sat, ...) ueber die offene
-- MediathekViewWeb-API. Pro Sender: Neueste, Sendungen, Kategorien; alles seitenweise
-- ("Weitere laden"), Vorschaubilder kommen von der jeweiligen Sendungsseite.

local json = require("json")
local API = "https://mediathekviewweb.de/api/query"
local PAGE = 50

-- Qualitaet: "low", "normal" (meist 960x540 - passt zur Vita), "hd" (720p/1080p - 1080p geht nicht)
local QUALITY = "normal"

local CHANNELS = { "ARD", "ZDF", "arte.de", "3Sat", "PHOENIX", "ZDFneo", "ZDFinfo", "ZDFtivi", "KiKA",
                   "BR", "NDR", "WDR", "SWR", "MDR", "HR", "RBB", "SR", "radiobremen", "ONE", "ARD-alpha",
                   "tagesschau24", "Funk.net", "DW", "ORF", "SRF" }

-- Kategorien: Suchbegriff (Thema/Titel/Beschreibung) und Mindestlaenge in Minuten
local CATEGORIES = {
  { "Filme",              "film",        70 },
  { "Dokumentationen",    "doku",        15 },
  { "Krimis",             "krimi",       40 },
  { "Tatort & Polizeiruf","tatort",      60 },
  { "Serien",             "folge",       15 },
  { "Comedy & Satire",    "satire",       5 },
  { "Kinder",             "kinder",       5 },
  { "Anime & Zeichentrick","anime",       5 },
  { "Nachrichten",        "nachrichten",  5 },
  { "Wissen",             "wissen",      10 },
  { "Musik & Konzerte",   "konzert",     20 },
  { "Sport",              "sport",        5 },
  { "Reisen",             "reise",       15 },
  { "Kochen",             "koch",        10 },
}

local function enc(s) return (s:gsub("[^%w%-_%.~ ]", function(c) return string.format("%%%02X", c:byte()) end)) end
local function dec(s) return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end

local function fmt_duration(sec)
  sec = tonumber(sec) or 0
  if sec >= 3600 then return string.format("%d:%02d:%02d", sec // 3600, (sec // 60) % 60, sec % 60) end
  return string.format("%d:%02d", sec // 60, sec % 60)
end

local function fmt_date(ts)
  ts = tonumber(ts)
  if not ts then return "" end
  local days = ts // 86400
  local z = days + 719468
  local era = (z >= 0 and z or z - 146096) // 146097
  local doe = z - era * 146097
  local yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
  local y = yoe + era * 400
  local doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
  local mp = (5 * doy + 2) // 153
  local d = doy - (153 * mp + 2) // 5 + 1
  local m = mp < 10 and mp + 3 or mp - 9
  if m <= 2 then y = y + 1 end
  return string.format("%02d.%02d.%d", d, m, y)
end

-- queries: Liste von { fields = {...}, query = "..." } (UND-verknuepft)
local function query(queries, offset, size, min_minutes, order)
  local body = json.encode({
    queries = queries,
    sortBy = "timestamp", sortOrder = order or "desc", future = false,
    offset = offset or 0, size = size or PAGE,
    duration_min = (min_minutes or 2) * 60,
  })
  local res, status = vs.http_post(API, body, "Content-Type: text/plain")
  if not res then return nil, status end
  local data, err = json.decode(res)
  if not data then return nil, "Antwort unlesbar: " .. tostring(err) end
  if data.err then return nil, "API-Fehler: " .. json.encode(data.err) end
  local r = data.result or {}
  local total = r.queryInfo and r.queryInfo.totalResults or #(r.results or {})
  return r.results or {}, total
end

local function to_item(r, show_channel)
  local url = r.url_video
  if QUALITY == "low" and r.url_video_low and #r.url_video_low > 0 then url = r.url_video_low end
  if QUALITY == "hd" and r.url_video_hd and #r.url_video_hd > 0 then url = r.url_video_hd end
  if not url or #url == 0 then return nil end
  local title = (r.topic and r.title and not r.title:find(r.topic, 1, true)) and (r.topic .. " - " .. r.title) or r.title
  local sub = string.format("%s%s  |  %s", show_channel and ((r.channel or "?") .. "  |  ") or "",
                            fmt_date(r.timestamp), fmt_duration(r.duration))
  return {
    title = title, subtitle = sub, id = url, kind = "video",
    thumb = (r.url_website and #r.url_website > 0) and ("og:" .. r.url_website) or nil,
    subs = (type(r.url_subtitle) == "string" and #r.url_subtitle > 0) and r.url_subtitle or nil,
  }
end

-- Ergebnisliste + "Weitere laden"
-- Staffel/Folge aus dem Titel: "(S02/E05)", "Staffel 2 Folge 5", "Folge 12", "Teil 3"
local function episode_key(title)
  local s, e = title:match("[Ss](%d+)%s*/%s*[Ee](%d+)")
  if not s then s, e = title:match("[Ss]taffel%s*(%d+).-[Ff]olge%s*(%d+)") end
  if not s then s, e = title:match("[Ss](%d+)[Ee](%d+)") end
  if s then return tonumber(s) * 10000 + tonumber(e) end
  local n = title:match("[Ff]olge%s*(%d+)") or title:match("[Tt]eil%s*(%d+)") or title:match("[Ee]pisode%s*(%d+)")
  return n and tonumber(n) or nil
end

local function results_page(queries, offset, min_minutes, more_prefix, show_channel, exact_topic)
  -- Sendereihen in Erscheinungsreihenfolge (aelteste zuerst), sonst neueste zuerst
  local res, total = query(queries, offset, PAGE, min_minutes, exact_topic and "asc" or "desc")
  if not res then return nil, total end
  local items = {}
  for _, r in ipairs(res) do
    -- Themen-Suche ist eine Teilwort-Suche: nur genau diese Sendereihe anzeigen
    if not exact_topic or r.topic == exact_topic then
      local it = to_item(r, show_channel)
      if it then
        if exact_topic then it.title = r.title end   -- Reihenname steht schon in der Kopfzeile
        items[#items + 1] = it
      end
    end
  end
  -- innerhalb der Seite nach Staffel/Folge ordnen, wenn alle Titel eine Nummer haben
  if exact_topic and #items > 1 then
    local all = true
    for i, it in ipairs(items) do
      it._k = episode_key(it.title)
      it._pos = i
      if not it._k then all = false end
    end
    if all then table.sort(items, function(a, b) if a._k ~= b._k then return a._k < b._k end return a._pos < b._pos end) end
  end
  local shown = offset + #res
  if #res == PAGE and (not total or shown < total) then
    items[#items + 1] = {
      title = total and string.format("Weitere laden (%d von %d)", shown, total) or "Weitere laden",
      kind = "more", id = more_prefix .. shown,
    }
  end
  if #items == 0 then return nil, "Keine Sendungen gefunden" end
  return items
end

-- Wie results_page, aber Folgen derselben Sendereihe (Thema + Sender) in einem Ordner
local GROUP_PAGE = 200
local function grouped_page(queries, offset, min_minutes, more_prefix, show_channel)
  local res, total = query(queries, offset, GROUP_PAGE, min_minutes)
  if not res then return nil, total end
  local order, groups = {}, {}
  for _, r in ipairs(res) do
    local key = (r.channel or "") .. "\1" .. (r.topic or "")
    if not groups[key] then groups[key] = {}; order[#order + 1] = key end
    table.insert(groups[key], r)
  end
  local items = {}
  for _, key in ipairs(order) do
    local g = groups[key]
    local r = g[1]
    if #g == 1 or not r.topic or #r.topic == 0 then
      for _, x in ipairs(g) do
        local it = to_item(x, show_channel)
        if it then items[#items + 1] = it end
      end
    else
      items[#items + 1] = {
        title = r.topic, kind = "folder",
        subtitle = string.format("%d Folgen  |  %s  |  neueste %s", #g, r.channel or "?", fmt_date(r.timestamp)),
        id = "topic:" .. enc(r.channel or "") .. ":" .. enc(r.topic) .. ":0",
        thumb = (r.url_website and #r.url_website > 0) and ("og:" .. r.url_website) or nil,
      }
    end
  end
  local shown = offset + #res
  if #res == GROUP_PAGE and (not total or shown < total) then
    items[#items + 1] = {
      title = total and string.format("Weitere Sendungen laden (%d von %d Beitraegen)", shown, total) or "Weitere laden",
      kind = "more", id = more_prefix .. shown,
    }
  end
  if #items == 0 then return nil, "Keine Sendungen gefunden" end
  return items
end

local function ch_query(ch)
  if ch == "" then return {} end
  return { { fields = { "channel" }, query = ch } }
end

local function with(base, extra)
  local q = {}
  for _, v in ipairs(base) do q[#q + 1] = v end
  q[#q + 1] = extra
  return q
end

local function folder(title, id, sub) return { title = title, id = id, kind = "folder", subtitle = sub } end

-- ================================================================ Senderliste (dynamisch)
-- Einmal pro App-Start: Sender aus den neuesten Beitraegen ermitteln und mit der
-- gespeicherten Liste zusammenfuehren. Ist MediathekViewWeb nicht erreichbar, gilt die
-- zuletzt gespeicherte Liste, sonst die eingebaute.
local CH_FILE = "mediathek_channels.txt"
local dyn_channels, dyn_state = nil, nil

local function channel_list()
  if dyn_channels then return dyn_channels, dyn_state end
  local seen, list = {}, {}
  local function add(ch)
    if ch and #ch > 0 and not seen[ch:lower()] then seen[ch:lower()] = true; list[#list + 1] = ch end
  end
  for _, ch in ipairs(CHANNELS) do add(ch) end
  local saved = vs.read_file(CH_FILE)
  local res = query({}, 0, 1500, 0)
  local found = {}
  if res then
    for _, r in ipairs(res) do if r.channel then found[#found + 1] = r.channel end end
  end
  if saved then for line in saved:gmatch("[^\r\n]+") do found[#found + 1] = line end end
  table.sort(found, function(a, b) return a:lower() < b:lower() end)
  for _, ch in ipairs(found) do add(ch) end
  if res then
    vs.write_file(CH_FILE, table.concat(list, "\n") .. "\n")
    dyn_state = "aktualisiert"
  else
    dyn_state = saved and "gespeicherte Liste (Server nicht erreichbar)" or "eingebaute Liste (Server nicht erreichbar)"
  end
  dyn_channels = list
  return list, dyn_state
end

-- ================================================================ Internet Archive
-- Offener, legaler Katalog frei zugaenglicher Filme/Shows in vielen Sprachen
-- (advancedsearch-API + Metadaten). Dient als englische/kroatische/mehrsprachige Mediathek.
local IA_SEARCH = "https://archive.org/advancedsearch.php"
local IA_PAGE = 50

-- Sprachen: Anzeige -> Suchausdruck fuer das Feld "language"
local IA_LANGS = {
  en = { "English", 'language:("English" OR "eng")' },
  hr = { "Hrvatski", 'language:("Croatian" OR "Hrvatski" OR "hrv" OR "hr")' },
  fr = { "Francais", 'language:("French" OR "fra")' },
  es = { "Espanol",  'language:("Spanish" OR "spa")' },
  it = { "Italiano", 'language:("Italian" OR "ita")' },
  ru = { "Russkij",  'language:("Russian" OR "rus")' },
}
-- Kategorien fuer das Internet Archive: Anzeige + Zusatz zur Suche
local IA_CATS = {
  { "Filme & Shows",       'mediatype:(movies)' },
  { "Public-Domain-Klassiker", 'collection:(feature_films)' },
  { "Dokumentationen",     'mediatype:(movies) AND subject:(documentary OR Doku)' },
  { "Kinder & Zeichentrick", 'mediatype:(movies) AND subject:(children OR cartoon OR animation)' },
  { "Musik & Konzerte",    'mediatype:(movies) AND subject:(concert OR music)' },
}

local function ia_list(query_expr, lang_expr, page)
  local q = query_expr .. " AND " .. lang_expr
  -- vollstaendig kodieren (Leerzeichen, Klammern, Anfuehrungszeichen) - sonst lehnt curl die URL ab
  local url = IA_SEARCH .. "?q=" .. vs.urlencode(q)
           .. "&fl%5B%5D=identifier&fl%5B%5D=title&fl%5B%5D=year"
           .. "&sort%5B%5D=downloads%20desc&rows=" .. IA_PAGE .. "&page=" .. (page + 1) .. "&output=json"
  local body, status = vs.http_get(url)
  if not body then return nil, "Internet Archive nicht erreichbar (" .. tostring(status) .. ")" end
  local ok, data = pcall(json.decode, body)
  if not ok or type(data) ~= "table" or not data.response then return nil, "Antwort unlesbar" end
  local docs = data.response.docs or {}
  local items = {}
  for _, d in ipairs(docs) do
    if d.identifier then
      local title = type(d.title) == "table" and d.title[1] or d.title or d.identifier
      items[#items + 1] = {
        title = tostring(title), subtitle = d.year and ("Jahr " .. tostring(d.year)) or "Internet Archive",
        id = "iaitem:" .. d.identifier, kind = "folder",
        thumb = "https://archive.org/services/img/" .. d.identifier,
      }
    end
  end
  local total = data.response.numFound or #docs
  local shown = page * IA_PAGE + #docs
  return items, total, shown
end

local function ia_page(query_expr, lang_expr, page, more_prefix)
  local items, total, shown = ia_list(query_expr, lang_expr, page)
  if not items then return nil, total end
  if #items == 0 then return nil, "Nichts gefunden" end
  if #items == IA_PAGE and shown < total then
    items[#items + 1] = { title = string.format("Weitere laden (%d von %d)", shown, total),
                          kind = "more", id = more_prefix .. (page + 1) }
  end
  return items
end

-- natuerliche Sortierung: "Folge 2" vor "Folge 10"
local function natural_less(a, b)
  local function key(s)
    return (s:lower():gsub("%d+", function(d) return string.format("%012d", tonumber(d)) end))
  end
  return key(a) < key(b)
end

local function ia_len(v)
  if not v then return nil end
  local h, m, sec = tostring(v):match("^(%d+):(%d+):(%d+)")
  local total = h and (tonumber(h) * 3600 + tonumber(m) * 60 + tonumber(sec)) or tonumber(v)
  if not total or total <= 0 then return nil end
  total = math.floor(total)
  if total >= 3600 then return string.format("%d:%02d:%02d", total // 3600, (total // 60) % 60, total % 60) end
  return string.format("%d:%02d", total // 60, total % 60)
end

-- Alle abspielbaren Videos eines Archiv-Eintrags: je Original (Folge) die kleinste MP4-Fassung
local function ia_files(identifier)
  local body, status = vs.http_get("https://archive.org/metadata/" .. identifier)
  local ok, meta = pcall(json.decode, body or "")
  if not ok or type(meta) ~= "table" or type(meta.files) ~= "table" then
    return nil, "Internet Archive: keine Metadaten (" .. tostring(status) .. ")"
  end
  local groups, order = {}, {}
  for _, f in ipairs(meta.files) do
    local name = f.name or ""
    local low = name:lower()
    if (low:match("%.mp4$") or low:match("%.m4v$")) and not low:find("sample", 1, true) then
      local key = (f.source == "derivative" and f.original) or name
      key = key:gsub("%.[%w]+$", "")
      local size = tonumber(f.size) or 0
      local g = groups[key]
      if not g then g = {}; groups[key] = g; order[#order + 1] = key end
      if not g.name or (size > 0 and (g.size == 0 or size < g.size)) then
        g.name, g.size = name, size
        g.title = (type(f.title) == "string" and f.title ~= "") and f.title or nil
        g.len = f.length
      end
    end
  end
  local list = {}
  for _, k in ipairs(order) do groups[k].key = k; list[#list + 1] = groups[k] end
  table.sort(list, function(a, b) return natural_less(a.key, b.key) end)
  return list, meta
end

local function ia_url(identifier, name)
  return "https://archive.org/download/" .. identifier .. "/" .. (vs.urlencode(name):gsub("%%2F", "/"))
end

-- Eintrag oeffnen: ein Film -> ein Video, eine Serie -> alle Folgen
local function ia_item(identifier)
  local list, meta = ia_files(identifier)
  if not list then return nil, meta end
  if #list == 0 then return nil, "Keine abspielbare MP4-Datei in diesem Eintrag (nur andere Formate)" end
  local thumb = "https://archive.org/services/img/" .. identifier
  local main_title = meta.metadata and meta.metadata.title
  if type(main_title) == "table" then main_title = main_title[1] end
  local items = {}
  for i, g in ipairs(list) do
    local label = g.title or g.key:gsub("^.*/", ""):gsub("[_%.]+", " ")
    if #list == 1 and main_title then label = tostring(main_title) end
    local sub = {}
    if #list > 1 then sub[#sub + 1] = "Teil " .. i .. " von " .. #list end
    local l = ia_len(g.len)
    if l then sub[#sub + 1] = l end
    if g.size > 0 then sub[#sub + 1] = string.format("%.0f MB", g.size / 1048576) end
    items[#items + 1] = { title = label, subtitle = table.concat(sub, "  |  "), kind = "video",
                          id = "iafile:" .. identifier .. "/" .. g.name, thumb = thumb }
  end
  return items
end

-- Kompatibilitaet (gespeicherte Eintraege aus 0.7/0.8): erstes Video eines Eintrags
local function ia_resolve(identifier)
  local list, err = ia_files(identifier)
  if not list then return nil, err end
  if #list == 0 then return nil, "Internet Archive: keine abspielbare MP4-Datei" end
  return ia_url(identifier, list[1].name)
end

local function ia_search(lang, text, page)
  local L = IA_LANGS[lang]
  if not L then return nil, "Unbekannte Sprache" end
  local t = text:gsub('[()"]', " ")
  return ia_page('mediatype:(movies) AND (title:(' .. t .. ') OR subject:(' .. t .. '))',
                 L[2], page, "iaq:" .. lang .. ":" .. enc(text) .. ":")
end

local function ia_home(lang)
  local L = IA_LANGS[lang]
  if not L then return nil, "Unbekannte Sprache" end
  local items = {
    { title = "Suchen ...", subtitle = L[1] .. " im Internet Archive", id = "iasearch:" .. lang, kind = "search" },
  }
  for ci, c in ipairs(IA_CATS) do
    items[#items + 1] = folder(c[1], "ia:" .. lang .. ":" .. ci .. ":0")
  end
  return items
end

-- ================================================================

return {
  name = "Mediatheken (nach Sprache)",
  description = "Deutsche Mediatheken + freie Kataloge in anderen Sprachen - Dreieck zum Suchen",

  browse = function(id)
    if id == nil then
      return {
        { title = "Suchen (deutsche Mediatheken) ...", subtitle = "Sendung, Thema oder Titel", id = "search", kind = "search" },
        folder("Deutsch", "lang:de", "ARD, ZDF, arte, 3sat, ORF, SRF ... (MediathekViewWeb)"),
        folder("English", "lang:en", "Freie Filme & Shows (Internet Archive)"),
        folder("Hrvatski / Kroatisch", "lang:hr", "Freie Inhalte auf Kroatisch (Internet Archive)"),
        folder("Francais", "lang:fr", "Freie Inhalte auf Franzoesisch (Internet Archive)"),
        folder("Espanol", "lang:es", "Freie Inhalte auf Spanisch (Internet Archive)"),
        folder("Weitere Sprachen", "lang:more", "Italienisch, Russisch ... (Internet Archive)"),
      }
    end

    -- Sprach-Startseiten
    if id == "lang:de" then
      local chans, state = channel_list()
      local items = {
        { title = "Suchen ...", subtitle = "in allen deutschen Mediatheken", id = "search", kind = "search" },
        folder("Alle Sender", "ch:" .. enc(""), #chans .. " Sender - Liste " .. state),
      }
      for _, ch in ipairs(chans) do items[#items + 1] = folder(ch, "ch:" .. enc(ch)) end
      return items
    end
    if id == "lang:more" then
      return {
        folder("Italiano", "lang:it"), folder("Russkij", "lang:ru"),
        folder("Francais", "lang:fr"), folder("Espanol", "lang:es"),
      }
    end
    local lang = id:match("^lang:(%a%a)$")
    if lang then return ia_home(lang) end

    -- Internet-Archive-Kategorie / -Suche
    local ialang, iaci, iaoff = id:match("^ia:(%a%a):(%d+):(%d+)$")
    if ialang then
      local L, C = IA_LANGS[ialang], IA_CATS[tonumber(iaci)]
      if not (L and C) then return nil, "Unbekannt" end
      return ia_page(C[2], L[2], tonumber(iaoff), "ia:" .. ialang .. ":" .. iaci .. ":")
    end
    local iaid = id:match("^iaitem:(.+)$")
    if iaid then return ia_item(iaid) end
    local ialq, iaquery, iaqoff = id:match("^iaq:(%a%a):(.-):(%d+)$")
    if ialq then return ia_search(ialq, dec(iaquery), tonumber(iaqoff)) end

    -- Sender-Startseite: Neueste, Sendungen, Kategorien
    local ch = id:match("^ch:(.*)$")
    if ch then
      local e = ch
      local items = {
        folder("Neueste", "new:" .. e .. ":0", "Zuletzt veroeffentlicht"),
        folder("Sendungen", "topics:" .. e, "Nach Sendereihe sortiert"),
      }
      for ci, c in ipairs(CATEGORIES) do items[#items + 1] = folder(c[1], "cat:" .. e .. ":" .. ci .. ":0") end
      return items
    end

    local nch, noff = id:match("^new:(.-):(%d+)$")
    if nch then
      return grouped_page(ch_query(dec(nch)), tonumber(noff), 2, "new:" .. nch .. ":", dec(nch) == "")
    end

    local cch, ci, coff = id:match("^cat:(.-):(%d+):(%d+)$")
    if cch then
      local c = CATEGORIES[tonumber(ci)]
      local q = with(ch_query(dec(cch)), { fields = { "topic", "title", "description" }, query = c[2] })
      return grouped_page(q, tonumber(coff), c[3], "cat:" .. cch .. ":" .. ci .. ":", dec(cch) == "")
    end

    -- Sendereihen aus den neuesten Beitraegen
    local tch = id:match("^topics:(.*)$")
    if tch then
      vs.log("Sammle Sendungen ...")
      local res, err = query(ch_query(dec(tch)), 0, 600, 2)
      if not res then return nil, err end
      local count, order = {}, {}
      for _, r in ipairs(res) do
        local t = r.topic or ""
        if #t > 0 then
          if not count[t] then count[t] = 0; order[#order + 1] = t end
          count[t] = count[t] + 1
        end
      end
      table.sort(order, function(a, b) return a:lower() < b:lower() end)
      local items = {}
      for _, t in ipairs(order) do
        items[#items + 1] = folder(t, "topic:" .. tch .. ":" .. enc(t) .. ":0", count[t] .. " neue Beitraege")
      end
      if #items == 0 then return nil, "Keine Sendungen gefunden" end
      return items
    end

    local och, otopic, ooff = id:match("^topic:(.-):(.-):(%d+)$")
    if och then
      local topic = dec(otopic)
      local q = with(ch_query(dec(och)), { fields = { "topic" }, query = topic })
      local items, err = results_page(q, tonumber(ooff), 1, "topic:" .. och .. ":" .. otopic .. ":", false, topic)
      return items, err
    end

    local sq, soff = id:match("^search:(.-):(%d+)$")
    if sq then
      return grouped_page({ { fields = { "title", "topic" }, query = dec(sq) } }, tonumber(soff), 2,
                          "search:" .. sq .. ":", true)
    end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(text, ctx)
    local ialang = ctx and ctx:match("^iasearch:(%a%a)$")
    if ialang then
      local items, err = ia_search(ialang, text, 0)
      if not items then return nil, (err == "Nichts gefunden") and ("Nichts gefunden fuer: " .. text) or err end
      return items
    end
    local items, err = grouped_page({ { fields = { "title", "topic" }, query = text } }, 0, 2,
                                    "search:" .. enc(text) .. ":", true)
    if not items then return nil, err == "Keine Sendungen gefunden" and ("Nichts gefunden fuer: " .. text) or err end
    return items
  end,

  resolve = function(item)
    local fid, fname = item.id:match("^iafile:([^/]+)/(.+)$")
    if fid then return ia_url(fid, fname) end
    local ident = item.id:match("^iaplay:(.+)$")
    if ident then return ia_resolve(ident) end
    if item.subs then
      return { url = item.id, subtitles = { { label = "Deutsch (Untertitel des Senders)", url = item.subs } } }
    end
    return item.id
  end,
}
