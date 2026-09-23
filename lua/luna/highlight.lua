-- luna.highlight: ANSI syntax highlighting for the REPL.
--
-- Tokenizing comes from scintillua's lexers (the Scintilla lexer
-- engine, LPeg-based) — no hand-written tokenizer. This module is also
-- the plugin extension point for highlighting:
--
--   highlight.active   the current highlighter, text -> text
--   highlight.set(fn)  replace it (nil restores the default)
--   highlight.styles   scintillua tag -> ANSI escape, extendable
--
-- Everything degrades to plain text when colors are off, when the
-- lexer stack (lpeg / lexer) is unavailable, or when a replacement
-- highlighter raises.
local highlight = {}

local RESET = "\27[0m"

-- scintillua tag name -> ANSI SGR escape. Tags not listed render
-- uncolored ("whitespace.lua", "identifier", ...). Plugins can add or
-- override entries with highlight.set_style.
highlight.styles = {
    keyword             = "\27[1;31m", -- bold red
    string              = "\27[32m",   -- green
    number              = "\27[36m",   -- cyan
    comment             = "\27[90m",   -- bright black (dim)
    ["function"]        = "\27[1;33m", -- bold yellow
    ["function.builtin"] = "\27[1;33m",
    ["function.method"] = "\27[1;33m",
    constant            = "\27[35m",   -- magenta
    ["constant.builtin"] = "\27[35m",
    label               = "\27[35m",
    attribute           = "\27[35m",
    error               = "\27[1;31m",
}

function highlight.set_style(tag, code)
    highlight.styles[tag] = code
end

-- The Lua lexer is loaded lazily and once; any failure here (missing
-- lpeg, missing lexer files) keeps the identity highlighter.
local state = { tried = false, lex = nil }

local function ensure_lexer()
    if state.tried then
        return state.lex
    end
    state.tried = true
    -- scintillua derives its lexer-file search from package.path
    -- entries ending in /?.lua (each becomes a lexer directory), so one
    -- element both locates the engine and registers the definitions.
    local dir = _G.__LUNA_LEXERS_DIR
    if dir and type(package) == "table" then
        package.path = dir .. "/?.lua;" .. package.path
    end
    local ok, lexer = pcall(require, "lexer")
    if not ok or type(lexer) ~= "table" then
        return nil
    end
    local okl, lua_lex = pcall(lexer.load, "lua")
    if not okl or (type(lua_lex) ~= "table" and type(lua_lex) ~= "userdata") then
        return nil
    end
    state.lex = lua_lex
    return lua_lex
end

-- Default highlighter: lex the text into (tag, end_exclusive) spans
-- (scintillua's flat M.lex layout) and wrap each styled span with its
-- escape sequence.
local function default_highlight(text)
    local lex = ensure_lexer()
    if not lex then
        return text
    end
    local ok, tags = pcall(lex.lex, lex, text, 0)
    if not ok or type(tags) ~= "table" then
        return text
    end
    local out, pos = {}, 1
    for i = 1, #tags, 2 do
        local tag, span_end = tags[i], tags[i + 1]
        if type(tag) ~= "string" or type(span_end) ~= "number"
            or span_end <= pos or span_end > #text + 1 then
            break -- malformed output: keep what we have
        end
        local seg = text:sub(pos, span_end - 1)
        local code = highlight.styles[tag]
        if code then
            out[#out + 1] = code .. seg .. RESET
        else
            out[#out + 1] = seg
        end
        pos = span_end
    end
    if pos <= #text then
        out[#out + 1] = text:sub(pos)
    end
    return table.concat(out)
end

-- Extension point: replace via highlight.set(fn); nil restores default.
highlight.active = default_highlight

function highlight.set(fn)
    if fn ~= nil and type(fn) ~= "function" then
        error("highlight.set expects a function or nil", 2)
    end
    highlight.active = fn or default_highlight
end

function highlight.reset()
    highlight.active = default_highlight
end

-- render(text, colors): the single call site the REPL uses. No colors
-- -> untouched text; a raising highlighter -> untouched text.
function highlight.render(text, colors)
    if not colors then
        return text
    end
    local ok, res = pcall(highlight.active, text)
    if not ok or type(res) ~= "string" then
        return text
    end
    return res
end

-- Per-byte color map for the line editor's real-time highlighter
-- callback (replxx wants one color per codepoint; the C bridge walks
-- the UTF-8 and maps codepoints to their first byte). Values are
-- replxx ReplxxColor integers (16-color base); unlisted tags stay
-- default so the map stays sparse.
local replxx_codes = {
    keyword              = 9,  -- bright red
    string               = 10, -- bright green
    number               = 14, -- bright cyan
    comment              = 8,  -- gray
    ["function"]         = 11, -- yellow
    ["function.builtin"] = 11,
    ["function.method"]  = 11,
    constant             = 13, -- bright magenta
    ["constant.builtin"] = 13,
    label                = 13,
    attribute            = 13,
    error                = 9,
}

function highlight.color_map(text)
    local map = {}
    local lex = ensure_lexer()
    if lex then
        local ok, tags = pcall(lex.lex, lex, text, 0)
        if ok and type(tags) == "table" then
            local pos = 1
            for i = 1, #tags, 2 do
                local tag, span_end = tags[i], tags[i + 1]
                if type(tag) ~= "string" or type(span_end) ~= "number"
                    or span_end <= pos or span_end > #text + 1 then
                    break
                end
                local code = replxx_codes[tag]
                if code then
                    for b = pos, span_end - 1 do
                        map[b] = code
                    end
                end
                pos = span_end
            end
        end
    end
    return map
end

return highlight
