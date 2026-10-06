-- Website-Explorer
--
-- Ablauf:
--   1. Website eingeben  -> Startseite wird geladen, Suchfunktion der Seite erkannt
--                           (OpenSearch, Suchformular, WordPress ?s=), Inhaltslinks aufgelistet
--   2. Auf der Seite suchen -> die 5 besten Treffer (+ "Weitere Treffer")
--   3. Treffer oeffnen   -> alle Videos, eingebetteten Player und Player-Links der Seite als Liste,
--                           dazu Staffel-/Folgenlinks als Unterordner
--   4. Eintrag abspielen -> direkte MP4/HLS-Links sofort; Player offener Plattformen ueber deren
--                           oeffentliche Schnittstelle (archive.org, Vimeo, Dailymotion, PeerTube);
--                           andere Player nur, wenn der Videolink offen in der Seite steht.
--
-- Bitte nur fuer Inhalte nutzen, die du legal abrufen darfst.

local json = require("json")

local HISTORY = "history_explorer.txt"
local TOP = 5
local MAX_LINKS = 60

-- ---------------------------------------------------------------- Hilfen

local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end
local function host_of(u) return ((u:match("^https?://([^/?#]+)") or ""):lower():gsub("^www%.", "")) end
local function origin_of(u) return u:match("^(https?://[^/?#]+)") end

local function absolute(base, link)
  link = trim((vs.html_unescape(link):gsub("\\/", "/")))
  if link:match("^https?://") then return link end
  if link:match("^//") then return (base:match("^(https?:)") or "https:") .. link end
  local origin = origin_of(base)
  if not origin then return link end
  if link:sub(1, 1) == "/" then return origin .. link end
  if link:sub(1, 1) == "?" then return (base:gsub("[?#].*$", "")) .. link end
  local dir = base:gsub("[?#].*$", ""):match("^(.*/)") or (origin .. "/")
  if dir == "https://" or dir == "http://" then dir = origin .. "/" end
  return dir .. link
end

local function strip_tags(s)
  s = s:gsub("<[^>]*>", " ")
  return trim((vs.html_unescape(s):gsub("%s+", " ")))
end

local function attr(tag, name)
  return tag:match("%s" .. name .. '%s*=%s*"([^"]*)"') or tag:match("%s" .. name .. "%s*=%s*'([^']*)'")
      or tag:match("%s" .. name .. "%s*=%s*([^%s>\"']+)")
end

local function fetch(url, referer)
  local html, status, final = vs.http_get(url, referer and ("Referer: " .. referer) or nil)
  if not html then return nil, status end
  if status and status >= 400 then return nil, "HTTP " .. status end
  return html, final or url
end

local MEDIA_EXT = { m3u8 = "HLS", mp4 = "MP4", ts = "MPEG-TS", mp3 = "Audio", aac = "Audio", m4v = "MP4" }
local function media_type(url)
  local ext = url:gsub("[?#].*$", ""):lower():match("%.(%w+)$")
  return ext and MEDIA_EXT[ext]
end

local SKIP_EXT = { css = 1, js = 1, png = 1, jpg = 1, jpeg = 1, gif = 1, svg = 1, webp = 1, ico = 1,
                   pdf = 1, zip = 1, xml = 1, json = 1, woff = 1, woff2 = 1, ttf = 1, rss = 1 }

local SKIP_PATH = { "login", "register", "signup", "anmelden", "registrieren", "impressum", "datenschutz",
                    "privacy", "contact", "kontakt", "dmca", "terms", "agb", "faq", "cookie", "feed",
                    "wp%-admin", "wp%-login", "/tag/", "/author/", "mailto:", "javascript:" }

local function skip_link(u)
  local l = u:lower()
  local ext = l:gsub("[?#].*$", ""):match("%.(%w+)$")
  if ext and SKIP_EXT[ext] then return true end
  for _, p in ipairs(SKIP_PATH) do if l:find(p) then return true end end
  return false
end

local function page_title(html)
  local t = html:match('<meta[^>]-property="og:title"[^>]-content="([^"]+)"')
         or html:match('<meta[^>]-content="([^"]+)"[^>]-property="og:title"')
         or html:match("<h1[^>]*>(.-)</h1>")
         or html:match("<title[^>]*>(.-)</title>")
  return t and strip_tags(t) or nil
end

local function page_image(html, base)
  local i = html:match('<meta[^>]-property="og:image"[^>]-content="([^"]+)"')
         or html:match('<meta[^>]-content="([^"]+)"[^>]-property="og:image"')
  return i and absolute(base, i) or nil
end

-- ---------------------------------------------------------------- Verlauf

local function load_history()
  local list = {}
  local txt = vs.read_file(HISTORY)
  if txt then for line in txt:gmatch("[^\r\n]+") do list[#list + 1] = line end end
  return list
end

local function remember(entry)
  local out = { entry }
  for _, u in ipairs(load_history()) do
    if u ~= entry and #out < 20 then out[#out + 1] = u end
  end
  vs.write_file(HISTORY, table.concat(out, "\n") .. "\n")
end

-- ---------------------------------------------------------------- Links einer Seite

-- alle <a>-Links mit Text: { url, text, img }
local function anchors(html, base)
  local out, seen = {}, {}
  for open, inner in html:gmatch("(<a%s[^>]->)(.-)</a>") do
    local href = attr(open, "href")
    if href and not href:match("^#") then
      local u = absolute(base, href):gsub("#.*$", "")
      if u:match("^https?://") and not seen[u] and not skip_link(u) then
        local text = strip_tags(inner)
        local img = inner:match("<img[^>]+>")
        if text == "" then text = attr(open, "title") or (img and (attr(img, "alt") or attr(img, "title"))) or "" end
        local src = img and (attr(img, "data%-src") or attr(img, "data%-lazy%-src") or attr(img, "src"))
        seen[u] = true
        out[#out + 1] = { url = u, text = trim(text), img = src and absolute(base, src) or nil }
      end
    end
  end
  return out
end

local CONTENT_WORDS = { "film", "movie", "serie", "series", "show", "staffel", "season", "folge", "episode",
                        "video", "watch", "stream", "anime", "doku", "sendung", "clip", "trailer", "kapitel", "teil" }

local function content_score(a)
  local l, s = (a.url .. " " .. a.text):lower(), 0
  for _, w in ipairs(CONTENT_WORDS) do if l:find(w, 1, true) then s = s + 2 end end
  if a.img then s = s + 3 end
  if #a.text >= 4 and #a.text <= 90 then s = s + 1 end
  if a.url:match("%d") then s = s + 1 end
  local _, slashes = a.url:gsub("^https?://[^/]+", ""):gsub("/", "")
  return s + math.min(slashes, 3)   -- tiefe Pfade sind eher Inhalte als Menuepunkte
end

-- ---------------------------------------------------------------- Suchfunktion erkennen

local SEARCH_NAMES = { q = 1, s = 1, query = 1, search = 1, keyword = 1, keywords = 1, k = 1, term = 1,
                       story = 1, suche = 1, searchword = 1, search_query = 1, find = 1 }

-- liefert { url = "...{q}...", method = "get"|"post", body = "...{q}...", how = "..." } oder nil
local function detect_search(html, base)
  -- 1. OpenSearch-Beschreibung
  for tag in html:gmatch("<link[^>]+>") do
    if (attr(tag, "type") or ""):find("opensearchdescription", 1, true) and attr(tag, "href") then
      local xml = fetch(absolute(base, attr(tag, "href")), base)
      if xml then
        for u in xml:gmatch("<Url[^>]+>") do
          local t, tmpl = attr(u, "type") or "", attr(u, "template")
          if tmpl and (t == "" or t:find("html", 1, true)) then
            tmpl = vs.html_unescape(tmpl):gsub("{searchTerms}", "{q}"):gsub("{[%w:]+%?}", "")
            return { url = absolute(base, tmpl), method = "get", how = "OpenSearch" }
          end
        end
      end
    end
  end
  -- 2. Suchformular
  for open, inner in html:gmatch("(<form[^>]*>)(.-)</form>") do
    local field, extra = nil, {}
    for input in inner:gmatch("<input[^>]+>") do
      local name, typ = attr(input, "name"), (attr(input, "type") or "text"):lower()
      if name then
        if not field and (typ == "search" or (typ == "text" and SEARCH_NAMES[name:lower()])) then
          field = name
        elseif typ == "hidden" then
          extra[#extra + 1] = vs.urlencode(name) .. "=" .. vs.urlencode(vs.html_unescape(attr(input, "value") or ""))
        end
      end
    end
    local role = ((attr(open, "role") or "") .. (attr(open, "id") or "") .. (attr(open, "class") or "") ..
                  (attr(open, "action") or "")):lower()
    if not field and role:find("search") then
      local input = inner:match("<input[^>]-type%s*=%s*[\"']?text[^>]*>") or inner:match("<input[^>]+>")
      field = input and attr(input, "name")
    end
    if field then
      local action = absolute(base, attr(open, "action") or base)
      local method = (attr(open, "method") or "get"):lower()
      table.insert(extra, 1, vs.urlencode(field) .. "={q}")
      local params = table.concat(extra, "&")
      if method == "post" then
        return { url = action, method = "post", body = params, how = "Formular (POST)" }
      end
      return { url = (action:gsub("%?.*$", "")) .. "?" .. params, method = "get", how = "Formular" }
    end
  end
  -- 3. WordPress
  if html:find("wp%-content") or html:find("wp%-json") then
    return { url = origin_of(base) .. "/?s={q}", method = "get", how = "WordPress" }
  end
  return nil
end

-- ---------------------------------------------------------------- Player auf einer Seite

local PLATFORM = {}   -- Name -> Resolver; unten befuellt

local function platform_of(u)
  local h = host_of(u)
  if h:find("archive%.org$") then return "archive.org" end
  if h:find("vimeo%.com$") then return "Vimeo" end
  if h:find("dailymotion%.com$") or h == "dai.ly" then return "Dailymotion" end
  if u:find("/videos/embed/", 1, true) or u:find("/videos/watch/", 1, true) or u:match("^https?://[^/]+/w/%w+") then
    return "PeerTube"
  end
  return nil
end

local PLAYER_ATTRS = { "data%-src", "data%-link", "data%-url", "data%-video", "data%-embed", "data%-player",
                       "data%-href", "data%-stream" }

local function looks_like_player(u)
  local l = u:lower()
  return l:find("/embed", 1, true) or l:find("/player", 1, true) or l:find("/video/", 1, true)
      or l:find("/videos/", 1, true) or l:find("/watch", 1, true) or platform_of(u) ~= nil
end

-- sammelt { url, label, kind = "media"|"player" }
local function collect_players(html, base)
  local out, seen = {}, {}
  local own = host_of(base)
  local function add(u, label, kind)
    if not u or u == "" then return end
    u = absolute(base, u)
    if not u:match("^https?://") or seen[u] then return end
    seen[u] = true
    out[#out + 1] = { url = u, label = label, kind = kind }
  end
  -- JSON in Skripten (Next.js & Co.): \/ und \u002F als Schraegstrich lesen
  local text = html:gsub("\\/", "/"):gsub("\\u002[Ff]", "/"):gsub("\\u0026", "&")
  -- direkte Medien
  for u in text:gmatch("(https?://[^\"'%s<>\\]+)") do
    if media_type(u) then add(u, nil, "media") end
  end
  for _, an in ipairs({ "src", "file", "data%-src" }) do
    for u in text:gmatch("%s" .. an .. '%s*=%s*["\']([^"\']+)["\']') do
      if media_type(u) then add(u, nil, "media") end
    end
  end
  for tag in html:gmatch("<meta[^>]+>") do
    local prop = (attr(tag, "property") or attr(tag, "name") or ""):lower()
    local content = attr(tag, "content")
    if prop == "og:video" or prop == "og:video:url" or prop == "og:video:secure_url" or prop == "twitter:player:stream" then
      add(content, nil, media_type(content or "") and "media" or "player")
    elseif prop == "twitter:player" then
      add(content, nil, "player")
    end
  end
  for u in text:gmatch('"contentUrl"%s*:%s*"([^"]+)"') do add(u, nil, media_type(u) and "media" or "player") end
  for u in text:gmatch('"embedUrl"%s*:%s*"([^"]+)"') do add(u, nil, "player") end
  -- eingebettete Player
  for tag in html:gmatch("<iframe[^>]+>") do
    add(attr(tag, "src") or attr(tag, "data%-src") or attr(tag, "data%-lazy%-src"), nil, "player")
  end
  -- Player-Auswahl (Buttons/Listen mit data-*-Attributen)
  for tag, inner in html:gmatch("(<%w+%s[^>]-data%-[^>]+>)([^<]*)") do
    for _, an in ipairs(PLAYER_ATTRS) do
      local v = attr(tag, an)
      if v and (v:match("^https?://") or v:match("^//")) then
        local label = strip_tags(inner)
        add(v, label ~= "" and label or attr(tag, "title"), "player")
      end
    end
  end
  -- Links zu Playern auf anderen Seiten
  for _, a in ipairs(anchors(html, base)) do
    if host_of(a.url) ~= own and looks_like_player(a.url) then
      add(a.url, a.text ~= "" and a.text or nil, "player")
    end
  end
  return out
end

local NAV_WORDS = { "staffel", "season", "folge", "episode", "ep%.", "teil", "part", "kapitel" }

local function nav_links(html, base)
  local out = {}
  local own = host_of(base)
  for _, a in ipairs(anchors(html, base)) do
    if host_of(a.url) == own and a.url ~= base then
      local l = (a.text .. " " .. a.url):lower()
      for _, w in ipairs(NAV_WORDS) do
        if l:find(w) then out[#out + 1] = a; break end
      end
    end
  end
  return out
end

-- ---------------------------------------------------------------- Ansichten

local cache = {}   -- Suchergebnisse fuer "Weitere Treffer"

local function site_url(q)
  q = trim(q)
  if not q:match("^https?://") then q = "https://" .. q end
  if not q:match("^https?://[^/]+/") then q = q .. "/" end
  return q
end

local function load_site_search(url, html)
  local key = "explorer_search_" .. host_of(url):gsub("[^%w%.%-]", "_") .. ".txt"
  local saved = vs.read_file(key)
  if saved then
    local method, u, body, how = saved:match("^(%a+)\t([^\t]*)\t([^\t]*)\t([^\t\r\n]*)")
    if method then return { method = method, url = u, body = body ~= "" and body or nil, how = how } end
  end
  if not html then html = fetch(url) end
  local s = html and detect_search(html, url)
  if s then vs.write_file(key, table.concat({ s.method, s.url, s.body or "", s.how }, "\t") .. "\n") end
  return s
end

local function link_item(a, sub)
  return { title = a.text ~= "" and a.text or a.url, subtitle = sub or host_of(a.url), id = "page:" .. a.url,
           kind = "folder", thumb = a.img or ("og:" .. a.url) }
end

local function open_site(url)
  vs.log("Lade " .. url)
  local html, final = fetch(url)
  if not html then return nil, "Seite nicht erreichbar: " .. url .. " (" .. tostring(final) .. ")" end
  url = final
  remember(origin_of(url) .. "/")
  local s = load_site_search(url, html)
  local items = {
    { title = "Auf " .. host_of(url) .. " suchen ...",
      subtitle = s and ("Suchfunktion erkannt: " .. s.how) or "Keine Suchfunktion erkannt - es werden die Links der Startseite durchsucht",
      id = "site:" .. url, kind = "search" },
    { title = "Diese Seite nach Videos durchsuchen", subtitle = url, id = "page:" .. url, kind = "folder" },
  }
  local own = host_of(url)
  local cand = {}
  for _, a in ipairs(anchors(html, url)) do
    if host_of(a.url) == own and a.url ~= url and a.text ~= "" then cand[#cand + 1] = a end
  end
  for i, a in ipairs(cand) do a.order = i; a.score = content_score(a) end
  table.sort(cand, function(a, b)
    if a.score ~= b.score then return a.score > b.score end
    return a.order < b.order
  end)
  for i = 1, math.min(#cand, MAX_LINKS) do items[#items + 1] = link_item(cand[i]) end
  return items
end

local function words(q)
  local w = {}
  for t in q:lower():gmatch("[%w\128-\255]+") do if #t > 1 then w[#w + 1] = t end end
  return w
end

local function match_score(a, ws, query)
  local text = a.text:lower()
  local slug = a.url:lower():gsub("[%-_/%.]", " ")
  local s = 0
  for _, w in ipairs(ws) do
    if text:find(w, 1, true) then s = s + 3 end
    if slug:find(w, 1, true) then s = s + 2 end
  end
  if text == query:lower() then s = s + 5 end
  if s > 0 and a.img then s = s + 1 end
  return s
end

-- Typische Such-Adressen, wenn die Seite kein erkennbares Suchformular hat
local GUESS = { "/?s={q}", "/search?q={q}", "/suche?q={q}", "/search/{q}", "/?q={q}", "/search?query={q}",
                "/search?keyword={q}", "/suche/{q}" }

local function guess_search(site, query)
  local origin = origin_of(site)
  local q = vs.urlencode(query):gsub("%%", "%%%%")
  local ws = words(query)
  local own = host_of(site)
  for _, g in ipairs(GUESS) do
    local url = origin .. (g:gsub("{q}", q))
    local html, final = fetch(url, site)
    if html then
      local hits = 0
      for _, a in ipairs(anchors(html, final)) do
        if host_of(a.url) == own and a.text ~= "" and match_score(a, ws, query) >= 3 then hits = hits + 1 end
      end
      if hits > 0 then
        local found = { url = origin .. g, method = "get", how = "erkannt: " .. g:gsub("{q}", "...") }
        local key = "explorer_search_" .. own:gsub("[^%w%.%-]", "_") .. ".txt"
        vs.write_file(key, table.concat({ found.method, found.url, "", found.how }, "\t") .. "\n")
        return found
      end
    end
  end
  return nil
end

local function site_search(site, query)
  local s = load_site_search(site)
  if not s then
    vs.log("Keine Suchfunktion erkannt - probiere typische Such-Adressen ...")
    s = guess_search(site, query)
  end
  local html, final
  if s then
    local q = vs.urlencode(query):gsub("%%", "%%%%")   -- als gsub-Ersatztext
    if s.method == "post" then
      local body, status, fu = vs.http_post(s.url, (s.body:gsub("{q}", q)),
                                 "Content-Type: application/x-www-form-urlencoded\nReferer: " .. site)
      if body and (not status or status < 400) then html, final = body, fu or s.url end
    else
      html, final = fetch((s.url:gsub("{q}", q)), site)
    end
  else
    html, final = fetch(site)   -- keine Suchfunktion: Startseite nach dem Begriff durchsuchen
  end
  if not html then return nil, "Suche fehlgeschlagen" end
  if type(final) ~= "string" then final = site end

  -- Treffer anhand des Begriffs ranken; Menuepunkte (Impressum, Kontakt ...)
  -- erzielen keinen Score und fallen damit heraus.
  local ws = words(query)
  local own = host_of(site)
  local hits = {}
  for _, a in ipairs(anchors(html, final)) do
    if host_of(a.url) == own and a.text ~= "" then
      local sc = match_score(a, ws, query)
      if sc > 0 then a.score = sc; hits[#hits + 1] = a end
    end
  end
  for i, a in ipairs(hits) do a.order = i end
  table.sort(hits, function(a, b)
    if a.score ~= b.score then return a.score > b.score end
    return a.order < b.order
  end)
  if #hits == 0 then
    return nil, "Keine Treffer fuer \"" .. query .. "\" auf " .. own ..
                (s and "" or " (keine Suchfunktion erkannt, nur Startseite durchsucht)")
  end
  local key = own .. "|" .. query
  cache[key] = hits
  local items = {}
  for i = 1, math.min(TOP, #hits) do items[#items + 1] = link_item(hits[i], "Treffer " .. i .. "  |  " .. own) end
  if #hits > TOP then
    items[#items + 1] = { title = "Weitere Treffer (" .. (#hits - TOP) .. ")", id = "more:" .. TOP .. ":" .. key, kind = "more" }
  end
  return items
end

local function more_hits(from, key)
  local hits = cache[key]
  if not hits then return nil, "Bitte die Suche erneut starten" end
  local own = key:match("^([^|]+)")
  local items = {}
  for i = from + 1, math.min(from + 20, #hits) do
    items[#items + 1] = link_item(hits[i], "Treffer " .. i .. "  |  " .. own)
  end
  if #hits > from + 20 then
    items[#items + 1] = { title = "Weitere Treffer (" .. (#hits - from - 20) .. ")", id = "more:" .. (from + 20) .. ":" .. key, kind = "more" }
  end
  return items
end

local function open_page(url)
  vs.log("Lade " .. url)
  local html, final = fetch(url)
  if not html then return nil, "Seite nicht erreichbar (" .. tostring(final) .. ")" end
  url = final
  local title = page_title(html) or url
  local image = page_image(html, url)
  local items = {}
  local players = collect_players(html, url)
  for i, p in ipairs(players) do
    local plat = p.kind == "player" and platform_of(p.url)
    local sub
    if p.kind == "media" then sub = (media_type(p.url) or "Video") .. "  |  " .. host_of(p.url)
    elseif plat then sub = "Player: " .. plat .. "  |  wird unterstuetzt"
    else sub = "Player: " .. host_of(p.url) .. "  |  nur mit offenem Videolink" end
    items[#items + 1] = {
      title = (p.label and p.label ~= "" and (title .. " - " .. p.label)) or
              (#players > 1 and (title .. " (" .. i .. ")") or title),
      subtitle = sub, id = p.url, url = p.url, kind = "video",
      page = url, thumb = image or ("og:" .. url),
    }
  end
  for _, a in ipairs(nav_links(html, url)) do
    items[#items + 1] = link_item(a, "Unterseite  |  " .. host_of(a.url))
  end
  if #items == 0 then
    return nil, "Auf dieser Seite wurden keine Videos oder Player gefunden. " ..
                "Viele Seiten laden ihre Player erst per JavaScript - das kann die App nicht ausfuehren."
  end
  return items
end

-- ---------------------------------------------------------------- offene Plattformen

PLATFORM["archive.org"] = function(u)
  local id = u:match("archive%.org/details/([^/?#]+)") or u:match("archive%.org/embed/([^/?#]+)")
  if not id then return nil end
  local body = vs.http_get("https://archive.org/metadata/" .. id)
  local ok, meta = pcall(json.decode, body or "")
  if not ok or type(meta) ~= "table" or type(meta.files) ~= "table" then return nil, "archive.org: keine Metadaten" end
  local best, best_size
  for _, f in ipairs(meta.files) do
    local name = f.name or ""
    if name:lower():match("%.mp4$") then
      local size = tonumber(f.size) or 0
      if not best or size < best_size then best, best_size = name, size end   -- meist die h.264-Ableitung
    end
  end
  if not best then return nil, "archive.org: keine MP4-Datei" end
  return "https://archive.org/download/" .. id .. "/" .. (vs.urlencode(best):gsub("%%2F", "/"))
end

PLATFORM["Vimeo"] = function(u, referer)
  local id = u:match("vimeo%.com/video/(%d+)") or u:match("vimeo%.com/(%d+)")
  if not id then return nil end
  local body = vs.http_get("https://player.vimeo.com/video/" .. id .. "/config",
                           "Referer: " .. (referer or "https://vimeo.com/"))
  local ok, cfg = pcall(json.decode, body or "")
  if not ok or type(cfg) ~= "table" or not cfg.request then return nil, "Vimeo: Video privat oder nicht einbettbar" end
  local files = cfg.request.files or {}
  local hls = files.hls
  if hls and hls.cdns then
    local c = hls.cdns[hls.default_cdn] or select(2, next(hls.cdns))
    if c and c.url then return c.url end
  end
  if files.progressive and files.progressive[1] then return files.progressive[1].url end
  return nil, "Vimeo: kein Stream gefunden"
end

PLATFORM["Dailymotion"] = function(u)
  local id = u:match("dailymotion%.com/embed/video/(%w+)") or u:match("dailymotion%.com/video/(%w+)")
          or u:match("dai%.ly/(%w+)") or u:match("[?&]video=(%w+)")
  if not id then return nil end
  local body = vs.http_get("https://www.dailymotion.com/player/metadata/video/" .. id)
  local ok, meta = pcall(json.decode, body or "")
  if not ok or type(meta) ~= "table" then return nil, "Dailymotion: keine Metadaten" end
  if meta.error then return nil, "Dailymotion: " .. tostring(meta.error.title or meta.error.message or "Fehler") end
  local auto = meta.qualities and meta.qualities.auto
  if auto and auto[1] and auto[1].url then return auto[1].url end
  return nil, "Dailymotion: kein Stream gefunden"
end

PLATFORM["PeerTube"] = function(u)
  local origin = origin_of(u)
  local id = u:match("/videos/embed/([%w%-]+)") or u:match("/videos/watch/([%w%-]+)") or u:match("/w/([%w%-]+)")
  if not (origin and id) then return nil end
  local body = vs.http_get(origin .. "/api/v1/videos/" .. id)
  local ok, v = pcall(json.decode, body or "")
  if not ok or type(v) ~= "table" then return nil, "PeerTube: keine Videodaten" end
  local sp = v.streamingPlaylists
  if type(sp) == "table" and sp[1] and sp[1].playlistUrl then return sp[1].playlistUrl end
  if type(v.files) == "table" then
    local best   -- hoechste Aufloesung bis 720p (Grenze der Vita)
    for _, f in ipairs(v.files) do
      local r = f.resolution and f.resolution.id or 0
      if r <= 720 and (not best or r > (best.resolution and best.resolution.id or 0)) then best = f end
    end
    if best then return best.fileUrl or best.fileDownloadUrl end
  end
  return nil, "PeerTube: kein Stream gefunden"
end

-- andere Player: nur offen sichtbare Videolinks der Player-Seite
local function open_player_page(u, referer)
  local html, final = fetch(u, referer)
  if not html then return nil, "Player nicht erreichbar" end
  for _, p in ipairs(collect_players(html, final)) do
    if p.kind == "media" then return p.url, final end
  end
  return nil, "Kein offener Videolink bei " .. host_of(u) .. " - dieser Player wird nicht unterstuetzt."
end

-- ---------------------------------------------------------------- Quelle

return {
  name = "Website-Explorer",
  description = "Website eingeben, darauf suchen, Videos und Player auflisten",

  browse = function(id)
    if id == nil then
      local items = { { title = "Website eingeben ...", subtitle = "z. B. archive.org", kind = "search" } }
      for _, e in ipairs(load_history()) do
        items[#items + 1] = { title = host_of(e), subtitle = e, id = "open:" .. e, kind = "folder", thumb = "og:" .. e }
      end
      return items
    end
    local site = id:match("^open:(.+)$")
    if site then return open_site(site) end
    local page = id:match("^page:(.+)$")
    if page then return open_page(page) end
    local from, key = id:match("^more:(%d+):(.+)$")
    if from then return more_hits(tonumber(from), key) end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(query, ctx)
    local q = trim(query or "")
    if q == "" then return nil, "Bitte etwas eingeben" end
    local site = ctx and ctx:match("^site:(.+)$")
    if site then return site_search(site, q) end
    if not q:match("%.") or q:match("%s") then
      return nil, "Bitte eine Webadresse eingeben (z. B. archive.org)"
    end
    return open_site(site_url(q))
  end,

  resolve = function(item)
    local u = item.url or item.id
    local referer = item.page
    if media_type(u) then return { url = u, headers = referer and { Referer = referer } or nil } end
    local plat = platform_of(u)
    if plat and PLATFORM[plat] then
      local stream, err = PLATFORM[plat](u, referer)
      if stream then return { url = stream } end
      if err then return nil, err end
    end
    local stream, page = open_player_page(u, referer)
    if not stream then return nil, page end
    return { url = stream, headers = { Referer = page } }
  end,
}
