-- Adult Swim (adultswim.com): kostenlos freigeschaltete Folgen
--
-- Katalog ueber die offene GraphQL-Schnittstelle der Website (/api/search), Stream ueber
-- /api/shows/v1/videos/<id>?fields=stream (HLS). Folgen, die einen US-TV-Anbieter verlangen
-- (auth), werden angezeigt, lassen sich aber nicht abspielen.
--
-- Grenzen: Das Angebot richtet sich an die USA, manche Folgen sind ausserhalb gesperrt.
-- Seit 2024 sind viele Streams mit Widevine verschluesselt - die meldet der Player als
-- kopiergeschuetzt (das kann die Vita nicht entschluesseln, und das wird auch nicht umgangen).

local json = require("json")

local API = "https://www.adultswim.com/api/search"
local UA = "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36"

-- bekannte Serien (Slug wie in adultswim.com/videos/<slug>)
local SHOWS = {
  { "Rick and Morty", "rick-and-morty" }, { "Smiling Friends", "smiling-friends" },
  { "Aqua Teen Hunger Force", "aqua-teen-hunger-force" }, { "Robot Chicken", "robot-chicken" },
  { "The Eric Andre Show", "the-eric-andre-show" }, { "Joe Pera Talks With You", "joe-pera-talks-with-you" },
  { "The Venture Bros.", "the-venture-bros" }, { "Metalocalypse", "metalocalypse" },
  { "Squidbillies", "squidbillies" }, { "Harvey Birdman, Attorney at Law", "harvey-birdman-attorney-at-law" },
  { "Space Ghost Coast to Coast", "space-ghost-coast-to-coast" }, { "Primal", "primal" },
  { "Sealab 2021", "sealab-2021" }, { "Home Movies", "home-movies" }, { "Tim and Eric Awesome Show", "tim-and-eric-awesome-show-great-job" },
  { "Your Pretty Face Is Going to Hell", "your-pretty-face-is-going-to-hell" }, { "Dream Corp LLC", "dream-corp-llc" },
  { "Birdgirl", "birdgirl" }, { "Common Side Effects", "common-side-effects" }, { "Lazor Wulf", "lazor-wulf" },
}

local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

local function gql(query)
  local body, status = vs.http_post(API, json.encode({ query = query }),
                                    "Content-Type: application/json\nAccept: application/json\n" .. UA)
  if not body then return nil, "Adult Swim nicht erreichbar: " .. tostring(status) end
  if status and status >= 400 then return nil, "Adult Swim antwortete HTTP " .. status end
  local ok, data = pcall(json.decode, body)
  if not ok or type(data) ~= "table" then return nil, "Antwort unlesbar" end
  if data.errors and not data.data then
    return nil, "Adult Swim: " .. tostring(data.errors[1] and data.errors[1].message or "Fehler")
  end
  return data.data
end

local function slugify(s)
  s = s:lower():gsub("&", "and"):gsub("[^%w%s%-]", ""):gsub("%s+", "-"):gsub("%-+", "-")
  return (s:gsub("^%-", ""):gsub("%-$", ""))
end

local function fmt_dur(sec)
  sec = tonumber(sec)
  if not sec or sec <= 0 then return nil end
  sec = math.floor(sec)
  return string.format("%d:%02d", sec // 60, sec % 60)
end

local function show_episodes(slug)
  local q = string.format([[query {
  getShowBySlug(slug:"%s") {
    title
    videos(first:1000,sort:["season_number","episode_number"]) {
      edges { node { _id slug title auth seasonNumber episodeNumber poster duration } }
    }
  }
}]], slug:gsub('[^%w%-]', ""))
  local data, err = gql(q)
  if not data then return nil, err end
  local show = data.getShowBySlug
  if not show then return nil, "Serie \"" .. slug .. "\" nicht gefunden" end
  local free, locked = {}, {}
  for _, e in ipairs((show.videos and show.videos.edges) or {}) do
    local v = e.node or {}
    if v._id then
      local label = ""
      if v.seasonNumber and v.episodeNumber then label = string.format("S%02dE%02d  ", tonumber(v.seasonNumber) or 0, tonumber(v.episodeNumber) or 0) end
      local sub = {}
      local d = fmt_dur(v.duration)
      if d then sub[#sub + 1] = d end
      if v.auth then sub[#sub + 1] = "nur mit US-TV-Anbieter" else sub[#sub + 1] = "kostenlos" end
      local it = { title = label .. (v.title or "?"), subtitle = table.concat(sub, "  |  "),
                   id = (v.auth and "locked:" or "v:") .. v._id, kind = "video", thumb = v.poster,
                   s = tonumber(v.seasonNumber) or 0, e = tonumber(v.episodeNumber) or 0 }
      table.insert(v.auth and locked or free, it)
    end
  end
  local function order(a, b) if a.s ~= b.s then return a.s < b.s end return a.e < b.e end
  table.sort(free, order)
  table.sort(locked, order)
  local items = {}
  for _, it in ipairs(free) do items[#items + 1] = it end
  for _, it in ipairs(locked) do items[#items + 1] = it end
  if #items == 0 then return nil, "Keine Folgen gefunden" end
  if #free == 0 then
    table.insert(items, 1, { title = "Derzeit keine kostenlosen Folgen", subtitle = (show.title or slug) ..
                             " - alle Folgen verlangen einen US-TV-Anbieter", id = "nop", kind = "folder" })
  end
  return items
end

return {
  name = "Adult Swim",
  save_ref = true,   -- Stream-Adressen laufen ab: in Playlists Verweis speichern
  description = "Kostenlose Folgen von adultswim.com (USA, teils kopiergeschuetzt)",

  browse = function(id)
    if id == nil then
      local items = { { title = "Serie suchen ...", subtitle = "Name der Serie, z. B. Rick and Morty", id = "search", kind = "search" } }
      for _, s in ipairs(SHOWS) do
        items[#items + 1] = { title = s[1], subtitle = "adultswim.com/videos/" .. s[2], id = "show:" .. s[2], kind = "folder" }
      end
      return items
    end
    if id == "nop" then return nil, "Nur Folgen mit \"kostenlos\" lassen sich abspielen" end
    local slug = id:match("^show:(.+)$")
    if slug then return show_episodes(slug) end
    return nil, "Unbekannter Eintrag"
  end,

  search = function(query)
    local q = trim(query or "")
    if q == "" then return nil, "Bitte einen Seriennamen eingeben" end
    return show_episodes(slugify(q))
  end,

  resolve = function(item)
    if item.id:match("^locked:") then
      return nil, "Diese Folge verlangt die Anmeldung bei einem US-TV-Anbieter."
    end
    local vid = item.id:match("^v:(.+)$") or item.id
    local body, status = vs.http_get("https://www.adultswim.com/api/shows/v1/videos/" .. vs.urlencode(vid) .. "?fields=stream",
                                     "Accept: application/json\n" .. UA)
    if not body then return nil, "Adult Swim nicht erreichbar: " .. tostring(status) end
    if status == 403 or status == 451 then return nil, "Adult Swim sperrt diese Folge in deiner Region (HTTP " .. status .. ")" end
    local ok, data = pcall(json.decode, body)
    local assets = ok and type(data) == "table" and data.data and data.data.video and data.data.video.stream
                   and data.data.video.stream.assets
    if type(assets) ~= "table" then
      return nil, "Kein freier Stream (Region USA oder Folge nicht freigeschaltet)"
    end
    local url
    local subs = {}
    for _, a in ipairs(assets) do
      local u = a.url or ""
      local mime = (a.mime_type or ""):lower()
      if not url and (u:lower():find("%.m3u8") or mime:find("mpegurl")) then url = u end
      if (u:lower():find("%.vtt") or mime:find("vtt")) and #subs < 2 then subs[#subs + 1] = { label = "Englisch", url = u } end
    end
    if not url then return nil, "Kein HLS-Stream fuer diese Folge" end
    return { url = url, headers = { Referer = "https://www.adultswim.com/", ["User-Agent"] = UA:sub(13) },
             subtitles = (#subs > 0) and subs or nil }
  end,
}
