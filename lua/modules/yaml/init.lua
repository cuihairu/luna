-- Transform between YAML 1.1 streams and Lua table representations.
-- Written by Gary V. Vaughan, 2013
--
-- Copyright(C) 2013-2026 Gary V. Vaughan
--
-- Permission is hereby granted, free of charge, to any person obtaining
-- a copy of this software and associated documentation files(the
-- "Software"), to deal in the Software without restriction, including
-- without limitation the rights to use, copy, modify, merge, publish,
-- distribute, sublicense, and/or sell copies of the Software, and to
-- permit persons to whom the Software is furnished to do so, subject to
-- the following conditions:
--
-- The above copyright notice and this permission notice shall be
-- included in all copies or substantial portions of the Software.
--
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
-- EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
-- MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
-- IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
-- CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
-- TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
-- SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
--
-- Portions of this software were inspired by an earlier LibYAML binding
-- by Andrew Danforth <acd@weirdness.net>

--- @module yaml
-- Staged copy of lyaml 6.2.9's lib/lyaml/init.lua (deps/lyaml), adapted
-- for luna: the requires below point at the staged helper copies and at
-- yaml.core (the C binding under its dotted registration name, so the
-- wrapper keeps "yaml"), and the export at the bottom is luna's module
-- face (decode/decodeAll/encode + the error/null contracts).


local explicit = require 'yaml.explicit'
local functional = require 'yaml.functional'
local implicit = require 'yaml.implicit'
local yaml = require 'yaml.core'

local NULL = functional.NULL
local anyof = functional.anyof
local find = string.find
local format = string.format
local gsub = string.gsub
local id = functional.id
local isnull = functional.isnull
local match = string.match


local TAG_PREFIX = 'tag:yaml.org,2002:'


local function tag(name)
   return TAG_PREFIX .. name
end


local default = {
   -- Tag table to lookup explicit scalar conversions.
   explicit_scalar = {
      [tag 'bool'] = explicit.bool,
      [tag 'float'] = explicit.float,
      [tag 'int'] = explicit.int,
      [tag 'null'] = explicit.null,
      [tag 'str'] = explicit.str,
   },
   -- Order is important, so we put most likely and fastest nearer
   -- the top to reduce average number of comparisons and funcalls.
   implicit_scalar = anyof {
      implicit.null,
      implicit.octal,	-- subset of decimal, must come earlier
      implicit.decimal,
      implicit.float,
      implicit.bool,
      implicit.inf,
      implicit.nan,
      implicit.hexadecimal,
      implicit.binary,
      implicit.sexagesimal,
      implicit.sexfloat,
      id,
   },
}


-- Metatable for Dumper objects.
local dumper_mt = {
   __index = {
      -- Emit EVENT to the LibYAML emitter.
      emit = function(self, event)
         return self.emitter.emit(event)
      end,

      -- Look up an anchor for a repeated document element.
      get_anchor = function(self, value)
         local r = self.anchors[value]
         if r then
            self.aliased[value], self.anchors[value] = self.anchors[value], nil
         end
         return r
      end,

      -- Look up an already anchored repeated document element.
      get_alias = function(self, value)
         return self.aliased[value]
      end,

      -- Dump ALIAS into the event stream.
      dump_alias = function(self, alias)
         return self:emit {
            type = 'ALIAS',
            anchor = alias,
         }
      end,

      -- Dump MAP into the event stream.
      dump_mapping = function(self, map)
         local alias = self:get_alias(map)
         if alias then
            return self:dump_alias(alias)
         end

         self:emit {
            type = 'MAPPING_START',
            anchor = self:get_anchor(map),
            style = 'BLOCK',
         }
         for k, v in pairs(map) do
            self:dump_node(k)
            self:dump_node(v)
         end
         return self:emit {type='MAPPING_END'}
      end,

      -- Dump SEQUENCE into the event stream.
      dump_sequence = function(self, sequence)
         local alias = self:get_alias(sequence)
         if alias then
            return self:dump_alias(alias)
         end

         self:emit {
            type   = 'SEQUENCE_START',
            anchor = self:get_anchor(sequence),
            style  = 'BLOCK',
         }
         for _, v in ipairs(sequence) do
            self:dump_node(v)
         end
         return self:emit {type='SEQUENCE_END'}
      end,

      -- Dump a null into the event stream.
      dump_null = function(self)
         return self:emit {
            type = 'SCALAR',
            value = '~',
            plain_implicit = true,
            quoted_implicit = true,
            style = 'PLAIN',
         }
      end,

      -- Dump VALUE into the event stream.
      dump_scalar = function(self, value)
         local alias = self:get_alias(value)
         if alias then
            return self:dump_alias(alias)
         end

         local anchor = self:get_anchor(value)
         local itsa = type(value)
         local style = 'PLAIN'
         if itsa == 'string' and self.implicit_scalar(value) ~= value then
            -- take care to round-trip strings that look like scalars
            style = 'SINGLE_QUOTED'
         elseif value == math.huge then
            value = '.inf'
         elseif value == -math.huge then
            value = '-.inf'
         elseif value ~= value then
            value = '.nan'
         elseif itsa == 'number' or itsa == 'boolean' then
            value = tostring(value)
         elseif itsa == 'string' and find(value, '\n') then
            style = 'LITERAL'
         end
         return self:emit {
            type = 'SCALAR',
            anchor = anchor,
            value = value,
            plain_implicit = true,
            quoted_implicit = true,
            style = style,
         }
      end,

      -- Decompose NODE into a stream of events.
      dump_node = function(self, node)
         local itsa = type(node)
         if isnull(node) then
            return self:dump_null()
         elseif itsa == 'string' or itsa == 'boolean' or itsa == 'number' then
            return self:dump_scalar(node)
         elseif itsa == 'table' then
            -- Something is only a sequence if its keys start at 1
            -- and are consecutive integers without any jumps.
            local prior_key = 0
            local is_pure_sequence = true
            local i, v = next(node, nil)
            while i and is_pure_sequence do
              if type(i) ~= "number" or (prior_key + 1 ~= i) then
                is_pure_sequence = false -- breaks the loop
              else
                prior_key = i
                i, v = next(node, prior_key)
              end
            end
            if is_pure_sequence then
               -- Only sequentially numbered integer keys starting from 1.
               return self:dump_sequence(node)
            else
               -- Table contains non sequential integer keys or mixed keys.
               return self:dump_mapping(node)
            end
         else -- unsupported Lua type
            error("cannot dump object of type '" .. itsa .. "'", 2)
         end
      end,

      -- Dump DOCUMENT into the event stream.
      dump_document = function(self, document)
         self:emit {type='DOCUMENT_START'}
         self:dump_node(document)
         return self:emit {type='DOCUMENT_END'}
      end,
   },
}


-- Emitter object constructor.
local function Dumper(opts)
   local anchors = {}
   for k, v in pairs(opts.anchors) do
      anchors[v] = k
   end
   local object = {
      aliased = {},
      anchors = anchors,
      emitter = yaml.emitter(),
      implicit_scalar = opts.implicit_scalar,
   }
   return setmetatable(object, dumper_mt)
end


--- Dump options table.
-- @table dumper_opts
-- @tfield table anchors map initial anchor names to values
-- @tfield function implicit_scalar parse implicit scalar values


--- Dump a list of Lua tables to an equivalent YAML stream.
-- @tparam table documents a sequence of Lua tables.
-- @tparam[opt] dumper_opts opts initialisation options
-- @treturn string equivalest YAML stream
local function dump(documents, opts)
   opts = opts or {}

   -- backwards compatibility
   if opts.anchors == nil and opts.implicit_scalar == nil then
      opts = {anchors=opts}
   end

   local dumper = Dumper {
      anchors = opts.anchors or {},
      implicit_scalar = opts.implicit_scalar or default.implicit_scalar,
   }

   dumper:emit {type='STREAM_START', encoding='UTF8'}
   for _, document in ipairs(documents) do
      dumper:dump_document(document)
   end
   local ok, stream = dumper:emit {type='STREAM_END'}
   return stream
end


-- We save anchor types that will match the node type from expanding
-- an alias for that anchor.
local alias_type = {
   MAPPING_END = 'MAPPING_END',
   MAPPING_START = 'MAPPING_END',
   SCALAR = 'SCALAR',
   SEQUENCE_END = 'SEQUENCE_END',
   SEQUENCE_START = 'SEQUENCE_END',
}


-- Metatable for Parser objects.
local parser_mt = {
   __index = {
      -- Return the type of the current event.
      type = function(self)
         return tostring(self.event.type)
      end,

      -- Raise a parse error.
      error = function(self, errmsg, ...)
         error(format('%d:%d: ' .. errmsg, self.mark.line,
                      self.mark.column, ...), 0)
      end,

      -- Save node in the anchor table for reference in future ALIASes.
      add_anchor = function(self, node)
         if self.event.anchor ~= nil then
            self.anchors[self.event.anchor] = {
               type = alias_type[self.event.type],
               value = node,
            }
         end
      end,

      -- Fetch the next event.
      parse = function(self)
         local ok, event = pcall(self.next)
         if not ok then
            -- if ok is nil, then event is a parser error from libYAML
            self:error(gsub(event, ' at document: .*$', ''))
         end
         self.event = event
         self.mark = {
            line = self.event.start_mark.line + 1,
            column = self.event.start_mark.column + 1,
         }
         return self:type()
      end,

      -- Construct a Lua hash table from following events.
      load_map = function(self)
         local map = {}
         self:add_anchor(map)
         while true do
            local key = self:load_node()
            local tag = self.event.tag
            if tag then
               tag = match(tag, '^' .. TAG_PREFIX .. '(.*)$')
            end
            if key == nil then
               break
            end
            if key == '<<' or tag == 'merge' then
               tag = self.event.tag or key
               local node, event = self:load_node()
               if event == 'MAPPING_END' then
                  for k, v in pairs(node) do
                     if map[k] == nil then
                        map[k] = v
                     end
                  end

               elseif event == 'SEQUENCE_END' then
                  for i, merge in ipairs(node) do
                     if type(merge) ~= 'table' then
                        self:error("invalid '%s' sequence element %d: %s",
                           tag, i, tostring(merge))
                     end
                     for k, v in pairs(merge) do
                        if map[k] == nil then
                           map[k] = v
                        end
                     end
                  end

               else
                  if event == 'SCALAR' then
                     event = tostring(node)
                  end
                  self:error("invalid '%s' merge event: %s", tag, event)
               end
            else
               local value, event = self:load_node()
               if value == nil then
                  self:error('unexpected %s event', self:type())
               end
               map[key] = value
            end
         end
         return map, self:type()
      end,

      -- Construct a Lua array table from following events.
      load_sequence = function(self)
         local sequence = {}
         self:add_anchor(sequence)
         while true do
            local node = self:load_node()
            if node == nil then
               break
            end
            sequence[#sequence + 1] = node
         end
         return sequence, self:type()
      end,

      -- Construct a primitive type from the current event.
      load_scalar = function(self)
         local value = self.event.value
         local tag = self.event.tag
         local explicit = self.explicit_scalar[tag]

         -- Explicitly tagged values.
         if explicit then
            value = explicit(value)
            if value == nil then
               self:error("invalid '%s' value: '%s'", tag, self.event.value)
            end

         -- Otherwise, implicit conversion according to value content.
         elseif self.event.style == 'PLAIN' then
            value = self.implicit_scalar(self.event.value)
         end
         self:add_anchor(value)
         return value, self:type()
      end,

      load_alias = function(self)
         local anchor = self.event.anchor
         local event = self.anchors[anchor]
         if event == nil then
            self:error('invalid reference: %s', tostring(anchor))
         end
         return event.value, event.type
      end,

      load_node = function(self)
         local dispatch = {
            SCALAR = self.load_scalar,
            ALIAS = self.load_alias,
            MAPPING_START = self.load_map,
            SEQUENCE_START = self.load_sequence,
            MAPPING_END = function() end,
            SEQUENCE_END = function() end,
            DOCUMENT_END = function() end,
         }

         local event = self:parse()
         if dispatch[event] == nil then
            self:error('invalid event: %s', self:type())
         end
       return dispatch[event](self)
      end,
   },
}


-- Parser object constructor.
local function Parser(s, opts)
   local object = {
      anchors = {},
      explicit_scalar = opts.explicit_scalar,
      implicit_scalar = opts.implicit_scalar,
      mark = {line=0, column=0},
      next = yaml.parser(s),
   }
   return setmetatable(object, parser_mt)
end


--- Load options table.
-- @table loader_opts
-- @tfield boolean all load all documents from the stream
-- @tfield table explicit_scalar map full tag-names to parser functions
-- @tfield function implicit_scalar parse implicit scalar values


--- Load a YAML stream into a Lua table.
-- @tparam string s YAML stream
-- @tparam[opt] loader_opts opts initialisation options
-- @treturn table Lua table equivalent of stream *s*
local function load(s, opts)
   opts = opts or {}
   local documents = {}
   local all = false

   -- backwards compatibility
   if opts == true then
      opts = {all=true}
   end

   local parser = Parser(s, {
      explicit_scalar = opts.explicit_scalar or default.explicit_scalar,
      implicit_scalar = opts.implicit_scalar or default.implicit_scalar,
   })

   if parser:parse() ~= 'STREAM_START' then
      error('expecting STREAM_START event, but got ' .. parser:type(), 2)
   end

   while parser:parse() ~= 'STREAM_END' do
      local document = parser:load_node()
      if document == nil then
         error('unexpected ' .. parser:type() .. ' event')
      end

      if parser:parse() ~= 'DOCUMENT_END' then
         error('expecting DOCUMENT_END event, but got ' .. parser:type(), 2)
      end

      -- save document
      documents[#documents + 1] = document

      -- reset anchor table
      parser.anchors = {}
   end

   return opts.all and documents or documents[1]
end


--[[ ----------------- ]]--
--[[  luna module face. ]]--
--[[ ----------------- ]]--

-- Error contract: luna's stdlib wants '<fmt>: <reason> at line N,
-- column M' (guide/modules.md). lyaml raises parser faults as
-- '<line>:<column>: <problem>' — the 1-based mark of the LAST EVENT THAT
-- PARSED successfully, not libyaml's problem_mark (upstream strips the
-- C message's own ", line: L, column: C" clause along with everything
-- after ' at document:'). Library faults arrive as plain strings, or
-- with a chunk 'file:line:' prefix from error(msg, n); those keep just
-- the reason, with no line clause — honest when no mark exists.
local function fail(msg)
   if type(msg) ~= 'string' then
      msg = tostring(msg)
   end
   local line, column, rest = match(msg, '^(%d+):(%d+):([^\n]*)')
   if line then
      return nil, format('yaml: %s at line %d, column %d',
                         match(rest, '^%s*(.-)%s*$'),
                         tonumber(line), tonumber(column))
   end
   local reason = match(msg, '^.-:%d+:%s*([^\n]*)$') or msg
   return nil, 'yaml: ' .. reason
end


-- Options are arguments, not data: a non-table opts raises (node-parity
-- 口径 2 — only bad DATA gets nil, err).
local function checkopts(opts, fname)
   if opts == nil then
      return {}
   end
   if type(opts) ~= 'table' then
      error(fname .. ': opts must be a table', 3)
   end
   return opts
end


-- null handling. lyaml carries its own NULL sentinel table through load
-- so a null VALUE stays distinguishable from a missing key while the
-- document is built. The luna contract turns it into nil by default
-- (the key is absent — json/dkjson semantics); opts.nullval substitutes
-- any other marker, yaml.null keeping lyaml's own sentinel. Only VALUES
-- are converted (a null KEY would silently delete its entry), and each
-- table is visited once, so aliases stay shared and a self-referential
-- anchor cannot loop the walk.
local function substitute(value, nullval, seen)
   if isnull(value) then
      return nullval
   end
   if type(value) ~= 'table' then
      return value
   end
   if seen[value] then
      return value
   end
   seen[value] = true
   for key, item in pairs(value) do
      if isnull(item) then
         value[key] = nullval
      elseif type(item) == 'table' then
         substitute(item, nullval, seen)
      end
   end
   return value
end


-- Decode one YAML document (the first when the stream holds several).
-- @tparam string s YAML stream
-- @tparam[opt] table opts {nullval = marker} plus lyaml's loader opts
-- @treturn[1] table|string|number|boolean decoded value
-- @treturn[2] nil, "yaml: <reason> at line N, column M"
local function decode(s, opts)
   opts = checkopts(opts, 'yaml.decode')
   if type(s) ~= 'string' then
      error('yaml.decode: expected a string', 2)
   end
   local ok, documents = pcall(load, s, opts)
   if not ok then
      return fail(documents)
   end
   return substitute(documents, opts.nullval, {})
end


-- Decode every document of a multi-document stream ('---' separated).
-- Same contracts as decode; returns an array (empty for an empty
-- stream). A null document lands as a nil slot under the default
-- nullval, and as the marker otherwise.
-- @tparam string s YAML stream
-- @tparam[opt] table opts {nullval = marker} plus lyaml's loader opts
-- @treturn table|nil, err
local function decodeAll(s, opts)
   opts = checkopts(opts, 'yaml.decodeAll')
   if type(s) ~= 'string' then
      error('yaml.decodeAll: expected a string', 2)
   end
   local lopts = {all = true}
   for key, item in pairs(opts) do
      lopts[key] = item
   end
   local ok, documents = pcall(load, s, lopts)
   if not ok then
      return fail(documents)
   end
   local nullval = opts.nullval
   for i = 1, #documents do
      documents[i] = substitute(documents[i], nullval, {})
   end
   return documents
end


-- Encode pre-walk: a table containing an ancestor of itself cannot be
-- emitted as YAML text. Shared siblings are fine (each occurrence
-- re-emits its own copy; anchor emission is a later batch) — only
-- ancestor loops are errors, the same口径 as dkjson's self-referential
-- check. Keys are walked too: a table can be a key.
local function cyclic(node, path, done)
   if type(node) ~= 'table' then
      return false
   end
   if path[node] then
      return true
   end
   if done[node] then
      return false
   end
   path[node] = true
   for key, item in pairs(node) do
      if cyclic(key, path, done) or cyclic(item, path, done) then
         return true
      end
   end
   path[node] = nil
   done[node] = true
   return false
end


-- Auto-anchor pre-walk (opts.anchors == true): collect every table the
-- dumper will encounter more than once — shared references, plus the
-- closure point of every ancestor cycle (an anchor is what terminates
-- emitting a cycle) — in first-encounter order, and name them a1..aN.
-- The names seed lyaml's predeclared-anchor map (name -> value), which
-- the Dumper inverts; from there the existing get_anchor/get_alias
-- machinery emits &aN on first occurrence and *aN for repeats, and
-- cycles terminate because the alias is registered at MAPPING/SEQUENCE
-- START, before the children are dumped. NULL sentinels dump as '~' and
-- are not tables to the dumper; table KEYS count too (a table can be a
-- key). Encounter order follows pairs, so with more than one shared
-- table the aN numbering is as stable as the mapping key order already
-- emitted by encode — same class, not a new one.
local function autoanchors(root)
   local counts, order, done = {}, {}, {}
   local walk
   local function edge(child, path)
      if type(child) == 'table' and not isnull(child) then
         if counts[child] == nil then
            order[#order + 1] = child
         end
         counts[child] = (counts[child] or 0) + 1
         walk(child, path)
      end
   end
   walk = function(node, path)
      if type(node) ~= 'table' or isnull(node) then
         return
      end
      if path[node] then
         -- cycle closure: the dumper re-encounters this node, so it
         -- needs an anchor even if no other edge points at it
         counts[node] = math.max(counts[node] or 0, 2)
         return
      end
      if done[node] then
         return
      end
      path[node] = true
      for key, item in pairs(node) do
         edge(key, path)
         edge(item, path)
      end
      path[node] = nil
      done[node] = true
   end
   walk(root, {})
   local names = {}
   local n = 0
   for _, node in ipairs(order) do
      if counts[node] > 1 then
         n = n + 1
         names[format('a%d', n)] = node
      end
   end
   return names
end


-- Encode one value as a YAML document.
-- @tparam any v the value (nil encodes to the empty stream, which
--   decodes back to nil)
-- @tparam[opt] table opts lyaml's dumper opts (anchors, implicit_scalar);
--   `anchors = true` switches to luna's auto-anchor mode: shared tables
--   (and cycle closure points) get a1..aN anchors and *aN aliases, so
--   decode restores reference identity and even ancestor cycles become
--   representable — the cyclic pre-error is skipped in this mode. The
--   lyaml predeclared form (`anchors = {name = value}`) keeps its
--   original meaning; the two shapes are told apart by type.
-- @treturn[1] string YAML text
-- @treturn[2] nil, err ("yaml: cyclic table reference", "yaml: cannot
--   dump object of type '<type>'", or "yaml: <reason> at line N, column M"
--   for emitter faults)
local function encode(v, opts)
   opts = checkopts(opts, 'yaml.encode')
   local dopts = opts
   if opts.anchors == true then
      dopts = {anchors = autoanchors(v),
               implicit_scalar = opts.implicit_scalar}
   elseif cyclic(v, {}, {}) then
      return nil, 'yaml: cyclic table reference'
   end
   local ok, stream = pcall(dump, {v}, dopts)
   if not ok then
      return fail(stream)
   end
   return stream
end


--- @export
return {
   decode = decode,
   decodeAll = decodeAll,
   encode = encode,

   --- `yaml.null` value (a table sentinel).
   -- @table null
   null = NULL,

   --- Version number from yaml C binding.
   -- @table _VERSION
   _VERSION = yaml.version,
}
