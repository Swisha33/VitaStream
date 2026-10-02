-- Kleiner JSON-Encoder/-Decoder fuer VitaStream-Plugins.
-- Nutzung:  local json = require("json")
--           local t = json.decode(text)
--           local s = json.encode({ a = 1, b = { 1, 2, 3 } })
-- Gibt eine Tabelle ohne "resolve" zurueck und wird daher nicht als Quelle geladen.

local json = {}
json.null = setmetatable({}, { __tostring = function() return "null" end })

-- ---------------- Decoder ----------------

local escapes = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }

local function utf8_char(cp)
  if cp < 0x80 then return string.char(cp) end
  if cp < 0x800 then return string.char(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)) end
  if cp < 0x10000 then
    return string.char(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
  end
  return string.char(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
end

local decode_value

local function skip_ws(s, i)
  return s:find("[^ \t\r\n]", i) or (#s + 1)
end

local function decode_error(s, i, msg)
  error(string.format("JSON-Fehler an Position %d: %s", i, msg), 0)
end

local function decode_string(s, i)
  -- s:sub(i,i) == '"'
  local out, j = {}, i + 1
  while true do
    local k = s:find('["\\]', j)
    if not k then decode_error(s, i, "String nicht beendet") end
    out[#out + 1] = s:sub(j, k - 1)
    if s:sub(k, k) == '"' then return table.concat(out), k + 1 end
    local c = s:sub(k + 1, k + 1)
    if c == "u" then
      local hex = s:sub(k + 2, k + 5)
      local cp = tonumber(hex, 16) or decode_error(s, k, "ungueltiges \\u")
      local nxt = k + 6
      if cp >= 0xD800 and cp <= 0xDBFF and s:sub(nxt, nxt + 1) == "\\u" then
        local lo = tonumber(s:sub(nxt + 2, nxt + 5), 16)
        if lo and lo >= 0xDC00 and lo <= 0xDFFF then
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
          nxt = nxt + 6
        end
      end
      out[#out + 1] = utf8_char(cp)
      j = nxt
    else
      out[#out + 1] = escapes[c] or decode_error(s, k, "ungueltiges Escape")
      j = k + 2
    end
  end
end

local function decode_array(s, i)
  local arr, n = {}, 0
  i = skip_ws(s, i + 1)
  if s:sub(i, i) == "]" then return arr, i + 1 end
  while true do
    local v
    v, i = decode_value(s, i)
    n = n + 1
    arr[n] = v
    i = skip_ws(s, i)
    local c = s:sub(i, i)
    if c == "]" then return arr, i + 1 end
    if c ~= "," then decode_error(s, i, "',' oder ']' erwartet") end
    i = skip_ws(s, i + 1)
  end
end

local function decode_object(s, i)
  local obj = {}
  i = skip_ws(s, i + 1)
  if s:sub(i, i) == "}" then return obj, i + 1 end
  while true do
    if s:sub(i, i) ~= '"' then decode_error(s, i, "Schluessel erwartet") end
    local k
    k, i = decode_string(s, i)
    i = skip_ws(s, i)
    if s:sub(i, i) ~= ":" then decode_error(s, i, "':' erwartet") end
    i = skip_ws(s, i + 1)
    local v
    v, i = decode_value(s, i)
    obj[k] = v
    i = skip_ws(s, i)
    local c = s:sub(i, i)
    if c == "}" then return obj, i + 1 end
    if c ~= "," then decode_error(s, i, "',' oder '}' erwartet") end
    i = skip_ws(s, i + 1)
  end
end

decode_value = function(s, i)
  i = skip_ws(s, i)
  local c = s:sub(i, i)
  if c == "{" then return decode_object(s, i) end
  if c == "[" then return decode_array(s, i) end
  if c == '"' then return decode_string(s, i) end
  if s:sub(i, i + 3) == "true" then return true, i + 4 end
  if s:sub(i, i + 4) == "false" then return false, i + 5 end
  if s:sub(i, i + 3) == "null" then return nil, i + 4 end
  local num = s:match("^-?%d+%.?%d*[eE]?[-+]?%d*", i)
  if num and #num > 0 then
    return tonumber(num) or decode_error(s, i, "ungueltige Zahl"), i + #num
  end
  decode_error(s, i, "unerwartetes Zeichen '" .. c .. "'")
end

function json.decode(s)
  if type(s) ~= "string" then return nil, "kein String" end
  local ok, res = pcall(function()
    local v, i = decode_value(s, 1)
    i = skip_ws(s, i)
    if i <= #s then decode_error(s, i, "Daten nach Ende") end
    return v
  end)
  if ok then return res end
  return nil, res
end

-- ---------------- Encoder ----------------

local enc_escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
                      ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }

local function encode(v, seen)
  local t = type(v)
  if v == nil or v == json.null then return "null" end
  if t == "boolean" then return tostring(v) end
  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    if math.type and math.type(v) == "integer" then return tostring(v) end
    return string.format("%.14g", v)
  end
  if t == "string" then
    return '"' .. v:gsub('[%c"\\]', function(c)
      return enc_escapes[c] or string.format("\\u%04x", c:byte())
    end) .. '"'
  end
  if t == "table" then
    if seen[v] then error("zyklische Tabelle") end
    seen[v] = true
    local out = {}
    if #v > 0 or next(v) == nil then
      for i = 1, #v do out[i] = encode(v[i], seen) end
      seen[v] = nil
      return "[" .. table.concat(out, ",") .. "]"
    end
    for k, val in pairs(v) do
      out[#out + 1] = encode(tostring(k), seen) .. ":" .. encode(val, seen)
    end
    seen[v] = nil
    return "{" .. table.concat(out, ",") .. "}"
  end
  error("nicht kodierbarer Typ: " .. t)
end

function json.encode(v) return encode(v, {}) end

return json
