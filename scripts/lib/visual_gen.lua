-- visual_gen.lua — generazione procedurale di piante d'interni (GUI_VISION.md §4)
--
-- Pipeline:
--   L1  spec   : { rooms={{id,type,size}}, edges={{a,b}}, entrance, style, seed }
--                (dal grafo world.lua o da una richiesta LLM)
--   L2  plan   : solver deterministico → rettangoli stanza + corridoio + porte
--   L3  tiler  : pianta → griglia di tile (usa catalog/walls.json)        [TODO]
--   L4  arredo : LLM sceglie oggetti dal catalogo → solver li piazza      [TODO]
--
-- L'LLM non emette mai coordinate: sceglie semantica (type, style, oggetti);
-- il solver fa la geometria, valida (flood-fill) e ripara. La pianta si
-- persiste nella location (snapshot world.lua) → generata una volta, stabile.
--
-- Test standalone:  luajit scripts/lib/visual_gen.lua   (stampa una pianta ASCII)

local M = {}

-- ── PRNG deterministico (LCG, seed → stessa pianta per sempre) ────────────────
local function rng(seed)
    local s = seed % 2147483647
    if s <= 0 then s = s + 2147483646 end
    return function(n)   -- ritorna intero in [1, n]
        s = (s * 16807) % 2147483647
        return 1 + math.floor((s / 2147483647) * n)
    end
end

-- hash stabile di una stringa (per derivare il seed dall'id location).
-- FNV-1a via libreria bit (LuaJIT); fallback aritmetico se assente.
local has_bit, bit = pcall(require, "bit")
function M.hash(str)
    if has_bit then
        local h = 2166136261
        for i = 1, #str do
            h = bit.bxor(h, str:byte(i))
            h = bit.band(h * 16777619, 0xFFFFFFFF)
        end
        return bit.band(h, 0x7FFFFFFF)
    end
    local h = 0
    for i = 1, #str do h = (h * 31 + str:byte(i)) % 2147483647 end
    return h
end

-- ── Dimensioni stanza per tipo/size ──────────────────────────────────────────
-- Interno utile in tile (esclusi i muri). depth = altezza, width = larghezza.
local SIZE_W = { S = {4, 5}, M = {6, 8}, L = {9, 12} }
local SIZE_H = { S = {3, 4}, M = {4, 6}, L = {6, 8} }
local TYPE_SIZE = {
    ingresso = "S", bagno = "S", ripostiglio = "S",
    cucina = "M", camera = "M", studio = "M", ufficio = "M",
    salotto = "L", soggiorno = "L", sala = "L",
}

local function room_dims(room, rnd)
    local sz = room.size or TYPE_SIZE[room.type or ""] or "M"
    local w = SIZE_W[sz]; local h = SIZE_H[sz]
    return w[1] + rnd(w[2] - w[1] + 1) - 1, h[1] + rnd(h[2] - h[1] + 1) - 1
end

-- ── L1: spec dal grafo (adapter per world.lua / TRAVEL_MAP) ──────────────────
-- Converte un cluster di location mono-piano in una spec per M.solve.
--   room_types : { [id] = "salotto"|"cucina"|... }  (tipo per dimensioni/arredo)
--   travel_map : { [id] = { vicino, ... } }          (grafo, archi simmetrici)
--   opts       : { entrance=id, style=, floor=, seed=, only={id=true} }
-- only: se presente, considera solo quelle location (il resto del grafo —
-- esterni, altri piani — è un altro stage). Gli archi verso location fuori
-- dal cluster sono ignorati (diventeranno transizioni di scena).
function M.build_spec(room_types, travel_map, opts)
    opts = opts or {}
    local only = opts.only
    local function inside(id) return (not only) or only[id] end

    local rooms, seen_edge, edges = {}, {}, {}
    for id, _ in pairs(room_types) do
        if inside(id) then
            rooms[#rooms+1] = { id = id, type = room_types[id] }
        end
    end
    for id, neigh in pairs(travel_map or {}) do
        if inside(id) then
            for _, n in ipairs(neigh) do
                if inside(n) then
                    local a, b = id, n
                    if a > b then a, b = b, a end
                    local key = a .. "|" .. b
                    if not seen_edge[key] then
                        seen_edge[key] = true
                        edges[#edges+1] = { a, b }
                    end
                end
            end
        end
    end
    return {
        seed     = opts.seed or M.hash(opts.entrance or (rooms[1] and rooms[1].id) or "x"),
        entrance = opts.entrance,
        style    = opts.style,
        floor    = opts.floor,
        rooms    = rooms,
        edges    = edges,
    }
end

-- ── L2: solver pianta — archetipo "corridoio" ────────────────────────────────
-- Spina orizzontale centrale; stanze appese sopra/sotto, ordinate per BFS dal
-- grafo a partire dall'ingresso. Ogni stanza ha una porta sul corridoio
-- (connettività garantita per costruzione). Le porte extra fra stanze adiacenti
-- nel grafo che risultano affiancate sono aggiunte dal tiler.
--
-- Geometria: ogni stanza è un blocco con muro nord spesso 2 righe (cap+face),
-- muro sud 1 riga, pareti verticali 1 colonna. Le coordinate in M.plan sono in
-- TILE, origine (0,0) in alto a sinistra dell'envelope.

-- BFS sul grafo per ordinare le stanze (ingresso prima)
local function bfs_order(rooms, edges, entrance)
    local adj = {}
    for _, r in ipairs(rooms) do adj[r.id] = {} end
    for _, e in ipairs(edges) do
        if adj[e[1]] and adj[e[2]] then
            table.insert(adj[e[1]], e[2]); table.insert(adj[e[2]], e[1])
        end
    end
    local start = entrance or rooms[1].id
    local seen, queue, order = { [start] = true }, { start }, {}
    while #queue > 0 do
        local id = table.remove(queue, 1)
        table.insert(order, id)
        for _, n in ipairs(adj[id] or {}) do
            if not seen[n] then seen[n] = true; table.insert(queue, n) end
        end
    end
    -- stanze non raggiungibili dal grafo: appendile comunque
    for _, r in ipairs(rooms) do
        if not seen[r.id] then table.insert(order, r.id); seen[r.id] = true end
    end
    return order, adj
end

-- WALL_N: il muro nord occupa 2 righe; SOUTH 1; pareti laterali 1 col.
local WALL_N, WALL_S, WALL_V, CORR_H = 2, 1, 1, 3   -- corridoio alto 3 tile

function M.solve(spec)
    local rnd = rng(spec.seed or 1)
    local by_id = {}
    for _, r in ipairs(spec.rooms) do by_id[r.id] = r end
    local order = bfs_order(spec.rooms, spec.edges or {}, spec.entrance)

    -- assegna ogni stanza a lato top/bottom alternando, dimensioni dal tipo
    local boxes = {}   -- { id, w, h (interni), side }
    for i, id in ipairs(order) do
        local r = by_id[id]
        local w, h = room_dims(r, rnd)
        boxes[#boxes+1] = { id = id, iw = w, ih = h,
                            side = (i % 2 == 1) and "top" or "bottom" }
    end

    -- spessori muro per ogni stanza
    local function full_w(b) return b.iw + 2 * WALL_V end
    local function full_h(b) return b.ih + WALL_N + WALL_S end

    -- colloca le stanze lungo X, top e bottom in due "nastri"
    local tops, bots = {}, {}
    for _, b in ipairs(boxes) do
        if b.side == "top" then tops[#tops+1] = b else bots[#bots+1] = b end
    end

    -- Struttura verticale (il corridoio è una stanza con muri propri):
    --   fascia top (stanze)           bottom-anchored, muro sud a top_band-1
    --   muro nord corridoio:  cap @ north_y, face @ north_y+1
    --   pavimento corridoio:  [floor_y .. floor_y+CORR_H-1]
    --   muro sud corridoio:   south @ south_y
    --   fascia bottom (stanze) muro nord da south_y+1
    local top_band = 0
    for _, b in ipairs(tops) do top_band = math.max(top_band, full_h(b)) end
    local north_y = top_band           -- riga cap del muro nord corridoio
    local floor_y = north_y + WALL_N   -- prima riga di pavimento corridoio
    local south_y = floor_y + CORR_H   -- riga muro sud corridoio

    local plan = { rooms = {}, doors = {}, corridor = nil, style = spec.style }

    -- top: bottom-anchored sopra il muro nord; bottom: sotto il muro sud.
    -- Porte: bucano il muro stanza + i muri del corridoio fino al pavimento.
    local function lay(strip)
        local x = 1
        for _, b in ipairs(strip) do
            local fw, fh = full_w(b), full_h(b)
            local y = (b.side == "top") and (north_y - fh) or (south_y + 1)
            plan.rooms[b.id] = {
                id = b.id, type = by_id[b.id].type,
                x = x, y = y, w = fw, h = fh,
                ix = x + WALL_V, iy = y + WALL_N,
                iw = b.iw, ih = b.ih, side = b.side,
            }
            local col = x + math.floor(fw / 2)
            local cells
            if b.side == "top" then
                -- muro sud stanza (y+fh-1) + muro nord corridoio (north_y, north_y+1)
                cells = { {col, y + fh - 1}, {col, north_y}, {col, north_y + 1} }
            else
                -- muro sud corridoio (south_y) + muro nord stanza (y, y+1)
                cells = { {col, south_y}, {col, y}, {col, y + 1} }
            end
            plan.doors[#plan.doors+1] = { room = b.id, col = col, cells = cells,
                                          dir = b.side == "top" and "south" or "north" }
            x = x + fw + 1
        end
        return x
    end

    local w_top = lay(tops)
    local w_bot = lay(bots)
    local total_w = math.max(w_top, w_bot, 4)

    plan.corridor = { x = 0, floor_y = floor_y, w = total_w, h = CORR_H,
                      north_y = north_y, south_y = south_y }
    plan.w = total_w
    plan.h = south_y + 1
    for _, b in ipairs(bots) do
        plan.h = math.max(plan.h, plan.rooms[b.id].y + plan.rooms[b.id].h)
    end
    plan.entrance = spec.entrance or order[1]
    return plan
end

-- ── Validazione: flood-fill reale dal corridoio ──────────────────────────────
-- Costruisce una griglia walkable (interni + corridoio + celle porta) e
-- verifica che ogni interno-stanza sia raggiungibile dal corridoio.
function M.validate_plan(plan)
    local walk = {}
    local function key(x, y) return y * 100000 + x end
    local function set(x, y) walk[key(x, y)] = true end
    -- corridoio (pavimento)
    local c = plan.corridor
    for y = c.floor_y, c.floor_y + c.h - 1 do
        for x = c.x + 1, c.x + c.w - 2 do set(x, y) end
    end
    -- interni
    for _, r in pairs(plan.rooms) do
        for y = r.iy, r.iy + r.ih - 1 do
            for x = r.ix, r.ix + r.iw - 1 do set(x, y) end
        end
    end
    -- celle porta (carve i muri)
    for _, d in ipairs(plan.doors) do
        for _, cell in ipairs(d.cells) do set(cell[1], cell[2]) end
    end
    -- flood-fill da una cella del corridoio
    local seen = {}
    local cstart = { c.x + 1, c.floor_y }
    local stack = { cstart }
    seen[key(cstart[1], cstart[2])] = true
    while #stack > 0 do
        local p = table.remove(stack)
        for _, d in ipairs({ {1,0},{-1,0},{0,1},{0,-1} }) do
            local nx, ny = p[1]+d[1], p[2]+d[2]
            local k = key(nx, ny)
            if walk[k] and not seen[k] then
                seen[k] = true; stack[#stack+1] = {nx, ny}
            end
        end
    end
    local issues = {}
    for id, r in pairs(plan.rooms) do
        if not seen[key(r.ix, r.iy)] then
            issues[#issues+1] = "irraggiungibile: " .. id
        end
    end
    return issues
end

-- ── L3: tiler — pianta → griglia di tile id (foglio combinato Room_Builder) ──
-- walls = catalog/walls.json decodificato. Ritorna grid[y][x] = local_id | -1.
-- Ogni stanza: muro nord 2 righe (cap+face) con angoli, pareti verticali,
-- muro sud 1 riga, pavimento interno. Le porte aprono il muro (pavimento).
function M.tile(plan, walls)
    local WR  = walls.wall_roles
    local FR  = walls.floor_roles
    local wb  = walls.wall_styles[plan.style] or select(2, next(walls.wall_styles))
    -- pavimento: default coerente con lo stile, o il primo disponibile
    local fb  = walls.floor_styles[plan.floor or ""]
             or walls.floor_styles["piastrelle_bianche"]
             or select(2, next(walls.floor_styles))

    local grid = {}
    for y = 0, plan.h - 1 do
        grid[y] = {}
        for x = 0, plan.w - 1 do grid[y][x] = -1 end
    end
    local function put(x, y, id) if grid[y] and x >= 0 and x < plan.w then grid[y][x] = id end end
    local function empty(x, y) return grid[y] and grid[y][x] == -1 end

    -- corridoio = stanza orizzontale con muri propri.
    --   muro nord: cap @ north_y, face @ north_y+1
    --   pavimento: [floor_y .. floor_y+h-1], pareti laterali vert
    --   muro sud:  south @ south_y
    local c = plan.corridor
    local cx0, cx1 = c.x, c.x + c.w - 1
    -- muro nord (cap+face) con angoli
    for x = cx0 + 1, cx1 - 1 do
        put(x, c.north_y,     wb + WR.cap_h)
        put(x, c.north_y + 1, wb + WR.face_h)
    end
    put(cx0, c.north_y,     wb + WR.corner_tl)
    put(cx1, c.north_y,     wb + WR.corner_tr)
    put(cx0, c.north_y + 1, wb + WR.face_tl)
    put(cx1, c.north_y + 1, wb + WR.face_tr)
    -- pavimento + pareti laterali
    for y = c.floor_y, c.floor_y + c.h - 1 do
        for x = cx0 + 1, cx1 - 1 do put(x, y, fb + FR.fill) end
        put(cx0, y, wb + WR.vert_w)
        put(cx1, y, wb + WR.vert_e)
    end
    -- muro sud con angoli
    for x = cx0 + 1, cx1 - 1 do put(x, c.south_y, wb + WR.south) end
    put(cx0, c.south_y, wb + WR.south_cl)
    put(cx1, c.south_y, wb + WR.south_cr)

    -- stanze
    for _, r in pairs(plan.rooms) do
        local x0, y0, w, h = r.x, r.y, r.w, r.h
        -- pavimento interno
        for y = r.iy, r.iy + r.ih - 1 do
            for x = r.ix, r.ix + r.iw - 1 do put(x, y, fb + FR.fill) end
        end
        -- muro nord: riga cap (y0) + riga face (y0+1)
        for x = x0 + 1, x0 + w - 2 do
            put(x, y0,     wb + WR.cap_h)
            put(x, y0 + 1, wb + WR.face_h)
        end
        put(x0,        y0,     wb + WR.corner_tl)
        put(x0 + w - 1, y0,    wb + WR.corner_tr)
        put(x0,        y0 + 1, wb + WR.face_tl)
        put(x0 + w - 1, y0 + 1, wb + WR.face_tr)
        -- pareti verticali (dopo il muro nord, fino a prima del sud)
        for y = y0 + 2, y0 + h - 2 do
            put(x0,        y, wb + WR.vert_w)
            put(x0 + w - 1, y, wb + WR.vert_e)
        end
        -- muro sud
        for x = x0 + 1, x0 + w - 2 do put(x, y0 + h - 1, wb + WR.south) end
        put(x0,        y0 + h - 1, wb + WR.south_cl)
        put(x0 + w - 1, y0 + h - 1, wb + WR.south_cr)
    end

    -- porte: apri l'opening (pavimento) e definisci gli stipiti ai lati.
    -- Per ogni cella porta su una riga di muro orizzontale, i vicini laterali
    -- diventano door_end (sx) / door_start (dx) sulla riga cap o face.
    for _, d in ipairs(plan.doors) do
        for _, cell in ipairs(d.cells) do
            local x, y = cell[1], cell[2]
            put(x, y, fb + FR.fill)
            -- riconosci se la riga è cap o face dal tile a sinistra/destra
            local function jamb(side_x, cap_id, face_id)
                local cur = grid[y] and grid[y][side_x]
                if cur == wb + WR.cap_h or cur == wb + WR.corner_tl
                   or cur == wb + WR.corner_tr then
                    put(side_x, y, wb + cap_id)
                elseif cur == wb + WR.face_h or cur == wb + WR.face_tl
                   or cur == wb + WR.face_tr then
                    put(side_x, y, wb + face_id)
                end
            end
            jamb(x - 1, WR.door_end_cap,   WR.door_end_face)
            jamb(x + 1, WR.door_start_cap, WR.door_start_face)
        end
    end

    -- apertura d'uscita: buca il muro perimetrale del corridoio
    if plan.exit then
        put(plan.exit.x, plan.exit.y, fb + FR.fill)
    end

    return grid
end

-- Serializza la griglia tiler (+ arredo opzionale) in JSON per il renderer.
-- furniture = lista piatta { {id,x,y,fw,fh,file}, ... } già risolta.
function M.tile_json(plan, walls, furniture)
    local json = require("lib/json")
    local grid = M.tile(plan, walls)
    local rows = {}
    for y = 0, plan.h - 1 do
        local row = {}
        for x = 0, plan.w - 1 do row[x+1] = grid[y][x] end
        rows[y+1] = row
    end
    return json.encode({ w = plan.w, h = plan.h, tiles = rows,
                         sheet = walls.sheet, cols = walls.sheet_cols,
                         furniture = furniture or {} })
end

-- ── L4: arredo — piazzamento deterministico di oggetti dal catalogo ──────────
-- items = { {id="sofa_blue_3seat", count=1}, ... } scelti dall'LLM per il tipo
-- stanza. catalog = catalog/objects.json decodificato (id → {footprint,wall_snap,..}).
-- Ritorna { {id, x, y, fw, fh}, ... } in coordinate tile. Regole:
--   wall_snap → contro il muro nord/sud/laterali, mai davanti a una porta;
--   liberi    → al centro con clearance; collisioni evitate via griglia occ.
-- Mai errore: se un oggetto non entra, viene saltato.
function M.furnish(plan, room_id, items, catalog, seed, fixtures)
    local r = plan.rooms[room_id]
    if not r then return {} end
    local rnd = rng((seed or 1) + M.hash(room_id))

    -- celle interne occupabili
    local occ = {}
    local function key(x, y) return y * 100000 + x end
    local function inside(x, y)
        return x >= r.ix and x < r.ix + r.iw and y >= r.iy and y < r.iy + r.ih
    end
    -- celle vietate: davanti alle porte (interno adiacente alla porta)
    for _, d in ipairs(plan.doors) do
        if d.room == room_id then
            for _, cell in ipairs(d.cells) do
                for _, dd in ipairs({{0,0},{0,1},{0,-1},{1,0},{-1,0}}) do
                    occ[key(cell[1]+dd[1], cell[2]+dd[2])] = "door"
                end
            end
        end
    end

    local function fits(x, y, fw, fh)
        for yy = y, y + fh - 1 do
            for xx = x, x + fw - 1 do
                if not inside(xx, yy) or occ[key(xx, yy)] then return false end
            end
        end
        return true
    end
    local function mark(x, y, fw, fh, id)
        for yy = y, y + fh - 1 do
            for xx = x, x + fw - 1 do occ[key(xx, yy)] = id end
        end
    end
    local function wall_slots(fw, fh)
        local slots = {}
        local top = r.iy
        for x = r.ix, r.ix + r.iw - fw do slots[#slots+1] = {x, top} end          -- nord
        local bot = r.iy + r.ih - fh
        for x = r.ix, r.ix + r.iw - fw do slots[#slots+1] = {x, bot} end           -- sud
        for y = r.iy, r.iy + r.ih - fh do slots[#slots+1] = {r.ix, y} end          -- ovest
        for y = r.iy, r.iy + r.ih - fh do slots[#slots+1] = {r.ix + r.iw - fw, y} end -- est
        return slots
    end
    local function free_slots(fw, fh)
        local slots = {}
        for y = r.iy + 1, r.iy + r.ih - fh - 1 do
            for x = r.ix + 1, r.ix + r.iw - fw - 1 do slots[#slots+1] = {x, y} end
        end
        return slots
    end

    local placed_fix = {}   -- fixture piazzati (riempito sotto)

    -- ── FIXTURE OBBLIGATORI (step 2b): arrangiamento per tipo-stanza ──────────
    -- Risolve una entry fixture in id (id esplicito o query sul catalogo).
    local function resolve(entry)
        if entry.id then return entry.id end
        local got = M.pick_furniture(catalog, entry, 1, seed)
        return got[1] and got[1].id
    end
    local function fw_of(id) return (catalog[id] and catalog[id].footprint or {1})[1] end

    if fixtures then
        -- run: fila contigua lungo il muro nord interno (es. cucina:
        -- piano|fornelli|piano|lavello). Salta le colonne davanti alle porte.
        if fixtures.run then
            local x = r.ix
            local row = r.iy
            for _, entry in ipairs(fixtures.run) do
                local id = resolve(entry)
                if id then
                    local w = fw_of(id)
                    -- avanza oltre colonne occupate (porte)
                    while x + w - 1 < r.ix + r.iw and not fits(x, row, w, 1) do x = x + 1 end
                    if x + w - 1 < r.ix + r.iw and fits(x, row, w, 1) then
                        mark(x, row, w, 1, id)
                        placed_fix[#placed_fix+1] = { id=id, x=x, y=row, fw=w, fh=1 }
                        x = x + w
                    end
                end
            end
        end
        -- wall: ogni fixture contro un muro libero (garantito se entra).
        for _, entry in ipairs(fixtures.wall or {}) do
            local id = resolve(entry)
            if id then
                local w = fw_of(id)
                for _, s in ipairs(wall_slots(w, 1)) do
                    if fits(s[1], s[2], w, 1) then
                        mark(s[1], s[2], w, 1, id)
                        placed_fix[#placed_fix+1] = { id=id, x=s[1], y=s[2], fw=w, fh=1 }
                        break
                    end
                end
            end
        end
    end

    -- classe di posizionamento di un oggetto (vincoli del passo 2):
    --   rug        → layer pavimento, sotto i mobili, sovrapponibile
    --   surface    → mobile che fornisce un piano d'appoggio (tavolo/bancone)
    --   on_surface → va SOPRA un piano (tostapane, lampada); query: on_surface=true
    --   floor      → mobile a terra normale (wall_snap o libero)
    local SURFACE_CAT = { table = true, counter = true, desk = true }
    local function classify(meta, it)
        if meta.category == "rug" then return "rug" end
        if it.on_surface then return "on_surface" end
        if SURFACE_CAT[meta.category] then return "surface" end
        return "floor"
    end

    -- espandi items, scartando non-arredabili (frammenti, UI, animati, archi).
    local BAD_KIND = { fragment = true, ui_or_effect = true }
    local rugs, floors, on_surf = {}, {}, {}
    for _, it in ipairs(items) do
        local meta = catalog[it.id]
        local ok = meta and not BAD_KIND[meta.kind or ""]
                        and (meta.complete == nil or meta.complete)
                        and not meta.animated
                        and meta.category ~= "door" and meta.category ~= "window"
        if ok then
            local fp = meta.footprint or {1, 1}
            for _ = 1, (it.count or 1) do
                local cls = classify(meta, it)
                local o = { id = it.id, fw = fp[1], fh = fp[2],
                            wall = meta.wall_snap, cls = cls,
                            provides = (cls == "surface") }
                if cls == "rug" then rugs[#rugs+1] = o
                elseif cls == "on_surface" then on_surf[#on_surf+1] = o
                else floors[#floors+1] = o end
            end
        end
    end
    -- mobili a terra: wall_snap prima, grandi prima
    table.sort(floors, function(a, b)
        if a.wall ~= b.wall then return a.wall end
        return (a.fw * a.fh) > (b.fw * b.fh)
    end)

    local placed = {}
    for _, p in ipairs(placed_fix) do placed[#placed+1] = p end   -- fixture per primi
    local surfaces = {}   -- piani disponibili: { {x,y,fw,fh}, ... }
    -- i piani della cucina (fixture run) sono superfici d'appoggio
    for _, p in ipairs(placed_fix) do
        local cat = (catalog[p.id] or {}).category
        if cat == "counter" or cat == "table" then
            surfaces[#surfaces+1] = { x=p.x, y=p.y, fw=p.fw, fh=p.fh }
        end
    end

    -- 1) MOBILI a terra (wall_snap + liberi); i "surface" diventano piani.
    --    Registra anche divani/letti per ancorarci il tappeto.
    local anchor   -- {x,y,fw,fh} di un divano/letto, per il tappeto
    for _, o in ipairs(floors) do
        local slots = o.wall and wall_slots(o.fw, o.fh) or free_slots(o.fw, o.fh)
        local start = #slots > 0 and rnd(#slots) or 1
        for i = 0, #slots - 1 do
            local s = slots[((start + i - 1) % #slots) + 1]
            if fits(s[1], s[2], o.fw, o.fh) then
                mark(s[1], s[2], o.fw, o.fh, o.id)
                placed[#placed+1] = { id = o.id, x = s[1], y = s[2], fw = o.fw, fh = o.fh }
                if o.provides then surfaces[#surfaces+1] = { x=s[1], y=s[2], fw=o.fw, fh=o.fh } end
                local cat = (catalog[o.id] or {}).category
                if not anchor and (cat == "seating" or cat == "bed") then
                    anchor = { x=s[1], y=s[2], fw=o.fw, fh=o.fh }
                end
                break
            end
        end
    end

    -- 1b) COPPIA ANCORATA: oggetto attaccato accanto all'ancora (comodino-letto).
    if fixtures and fixtures.pair and anchor then
        local id = resolve(fixtures.pair.attached or fixtures.pair)
        if id then
            local w = fw_of(id)
            -- prova a destra dell'ancora, poi a sinistra
            local cands = { {anchor.x + anchor.fw, anchor.y},
                            {anchor.x - w,          anchor.y} }
            for _, s in ipairs(cands) do
                if fits(s[1], s[2], w, 1) then
                    mark(s[1], s[2], w, 1, id)
                    placed[#placed+1] = { id=id, x=s[1], y=s[2], fw=w, fh=1 }
                    break
                end
            end
        end
    end

    -- 2) TAPPETI: sotto il divano/letto se c'è (ancoraggio), altrimenti al
    --    centro. layer="floor" → disegnati sotto i mobili, sovrapponibili.
    for _, o in ipairs(rugs) do
        local cx, cy
        if anchor then
            -- centra il tappeto sotto l'ancora (sporgendo un po' davanti)
            cx = anchor.x + math.floor((anchor.fw - o.fw) / 2)
            cy = anchor.y + anchor.fh - 1
        else
            cx = r.ix + math.floor((r.iw - o.fw) / 2)
            cy = r.iy + math.floor((r.ih - o.fh) / 2)
        end
        cx = math.max(r.ix, math.min(cx, r.ix + r.iw - o.fw))
        cy = math.max(r.iy, math.min(cy, r.iy + r.ih - o.fh))
        placed[#placed+1] = { id = o.id, x = cx, y = cy, fw = o.fw, fh = o.fh,
                              layer = "floor" }
    end

    -- 3) OGGETTI da appoggio: SOPRA un piano (centro del piano), layer="top".
    --    Se non c'è un piano, fallback a terra.
    for _, o in ipairs(on_surf) do
        local s = surfaces[#surfaces > 0 and rnd(#surfaces) or 1]
        if s then
            placed[#placed+1] = { id = o.id, x = s.x + math.floor((s.fw - o.fw)/2),
                                  y = s.y, fw = o.fw, fh = o.fh, layer = "top" }
        else
            for _, sl in ipairs(free_slots(o.fw, o.fh)) do
                if fits(sl[1], sl[2], o.fw, o.fh) then
                    mark(sl[1], sl[2], o.fw, o.fh, o.id)
                    placed[#placed+1] = { id = o.id, x = sl[1], y = sl[2], fw = o.fw, fh = o.fh }
                    break
                end
            end
        end
    end

    return placed
end

-- Rende un path asset assoluto: prepende ASSET_ROOT (global iniettato
-- dall'engine) ai path relativi, così la GUI li trova da qualunque cwd.
-- Lascia invariati i path già assoluti (/...) o home (~...).
function M.abs(path)
    if not path or path == "" then return path end
    local first = path:sub(1, 1)
    if first == "/" or first == "~" then return path end
    local root = rawget(_G, "ASSET_ROOT")
    if type(root) == "string" and root ~= "" then
        return root .. "/" .. path
    end
    return path
end

-- ── Selezione mobili dal catalogo (robusta: per categoria, non id hardcoded) ──
-- Gli id semantici del catalogo cambiano se si ri-genera col VLM; selezionare
-- per categoria/kind è stabile ed è anche ciò che fa l'LLM in produzione.
-- query: { category=, name_contains=, max_w= }. Ritorna fino a n id (mobili
-- completi, kind=furniture), deterministico dal seed.
function M.pick_furniture(catalog, query, n, seed)
    local theme_ok = query.themes and {} or nil
    if theme_ok then for _, t in ipairs(query.themes) do theme_ok[t] = true end end
    local matches = {}
    for id, e in pairs(catalog) do
        local ok = (e.kind == "furniture") and (e.complete ~= false)
        if ok and theme_ok and not theme_ok[e.theme or ""] then ok = false end
        if ok and query.category and e.category ~= query.category then ok = false end
        if ok and query.name_contains
           and not (e.name_it or ""):lower():find(query.name_contains, 1, true)
           and not id:lower():find(query.name_contains, 1, true) then ok = false end
        if ok and query.max_w and (e.footprint or {1})[1] > query.max_w then ok = false end
        if ok then matches[#matches+1] = id end
    end
    table.sort(matches)   -- ordine stabile
    local rnd = rng((seed or 1) + M.hash(query.category or query.name_contains or "x"))
    -- scegli n distinti pseudo-casuali ma deterministici
    local out = {}
    local count = math.min(n or 1, #matches)
    local used = {}
    while #out < count do
        local i = rnd(#matches)
        if not used[i] then
            used[i] = true
            out[#out+1] = { id = matches[i], on_surface = query.on_surface }
        end
    end
    return out
end

-- Costruisce una palette { [room_type] = { {id}, ... } } da una mappa di query
-- per tipo: { [room_type] = { {category=,n=}, ... } }.
-- default_themes: lista di temi preferiti applicata a ogni query priva di
-- themes (es. residenziali per una casa), per coerenza visiva.
function M.auto_palette(catalog, queries, seed, default_themes)
    local palette = {}
    for rtype, qlist in pairs(queries) do
        local items = {}
        for _, q in ipairs(qlist) do
            if default_themes and not q.themes then q.themes = default_themes end
            for _, it in ipairs(M.pick_furniture(catalog, q, q.n or 1, seed)) do
                items[#items+1] = it
            end
        end
        palette[rtype] = items
    end
    return palette
end

-- ── Architettura: porte e finestre (stile uniforme a livello edificio) ───────
-- Scelte UNA volta dal seed edificio → identiche in tutto l'appartamento.
-- Porte negli accessi stanza↔corridoio + una porta d'USCITA nel perimetro;
-- finestre sui muri esterni (faccia nord 3D). Statiche residenziali (gli
-- animati hanno footprint = strip di frame, gestiti a parte in seguito).
-- Ritorna lista di sprite { id, x, y, fw=1, fh=1 } (ancoraggio centrato 1 tile).
function M.place_architecture(plan, catalog, seed, opts)
    opts = opts or {}
    local themes = opts.themes
    local function pick(category)
        local q = { category = category, themes = themes, max_w = 2 }
        local cands = {}
        for id, e in pairs(catalog) do
            if e.category == category and not e.animated
               and (e.footprint or {1})[1] <= 2 then
                if not themes then cands[#cands+1] = id
                else
                    for _, t in ipairs(themes) do
                        if e.theme == t then cands[#cands+1] = id; break end
                    end
                end
            end
        end
        table.sort(cands)
        if #cands == 0 then return nil end
        local rnd = rng((seed or 1) + M.hash(category))
        return cands[rnd(#cands)]
    end

    local door_id   = opts.door   or pick("door")
    local window_id = opts.window or pick("window")
    local out = {}

    -- porte negli accessi (centrate sulla colonna porta, ancorate nel vano)
    if door_id then
        for _, d in ipairs(plan.doors) do
            local row = d.cells[1][2]
            for _, cell in ipairs(d.cells) do row = math.max(row, cell[2]) end
            out[#out+1] = { id = door_id, x = d.col, y = row, fw = 1, fh = 1 }
        end
    end

    -- porta d'uscita: apri il muro ovest del corridoio e mettici una porta
    local c = plan.corridor
    if door_id and c then
        local mid = c.floor_y + math.floor(c.h / 2)
        out[#out+1] = { id = door_id, x = c.x, y = mid, fw = 1, fh = 1, exit = true }
        plan.exit = { x = c.x, y = mid }   -- segnala l'apertura al tiler
    end

    -- finestre: una per stanza sul muro nord (faccia esterna). Usa il footprint
    -- reale (la finestra è larga 1-2 tile) e una colonna centrale, lontana dagli
    -- angoli; y abbassato così la finestra sta SUL muro (non fluttua sopra).
    if window_id then
        local wfp = (catalog[window_id] and catalog[window_id].footprint) or {2,1}
        local ww = wfp[1]
        for _, r in pairs(plan.rooms) do
            -- centra sul muro nord, almeno 1 tile dagli angoli
            local col = r.ix + math.max(1, math.floor((r.iw - ww) / 2))
            out[#out+1] = { id = window_id, x = col, y = r.y + 1,
                            fw = ww, fh = 1, window = true, layer = "top" }
        end
    end

    return out, door_id, window_id
end

-- ── Punti di transito (connettività): scale/uscite verso altri ambienti ──────
-- exits_spec: { [room_id] = { {to=loc_id, kind="stair"|"door"}, ... } }.
-- Le uscite INTRA-piano sono già aperture-porta nella pianta; qui si piazzano
-- gli oggetti-transito CROSS (scale verso un altro piano/ambiente) su un tile
-- preciso, e si registra plan.transit[room_id] = { {to, x, y}, ... } così la
-- GUI sa dove l'avatar entra/esce. Ritorna gli sprite (da unire ai mobili).
function M.place_exits(plan, exits_spec, catalog, opts)
    opts = opts or {}
    local stair_id = opts.stair or "staircase_stone"
    local sprites = {}
    plan.transit = {}
    for room_id, exits in pairs(exits_spec or {}) do
        local r = plan.rooms[room_id]
        if r then
            plan.transit[room_id] = {}
            local slot = 0
            for _, ex in ipairs(exits) do
                if ex.kind == "stair" then
                    local meta = catalog[stair_id]
                    local fw = (meta and meta.footprint or {3})[1]
                    -- contro il muro nord, a partire da sinistra, slot successivi
                    local col = r.ix + 1 + slot * (fw + 1)
                    local row = r.iy
                    if col + fw - 1 < r.ix + r.iw then
                        sprites[#sprites+1] = { id=stair_id, x=col, y=row,
                                                fw=fw, fh=1, layer="top", transit=ex.to }
                        table.insert(plan.transit[room_id],
                            { to=ex.to, x=col + math.floor(fw/2), y=row })
                        slot = slot + 1
                    end
                end
            end
        end
    end
    return sprites
end

-- Transit tile (stair / entrance) of a room — where an NPC entering this floor
-- from elsewhere should SPAWN before walking to its room. plan.transit[room] is
-- a list of { to, x, y }; pass `toward` to pick the exit leading to a specific
-- destination (the cluster/room the NPC came from), else the first one.
-- Returns { x, y } or nil if the room has no transit.
function M.transit_tile(plan, room_id, toward)
    local list = plan and plan.transit and plan.transit[room_id]
    if type(list) ~= "table" or #list == 0 then return nil end
    if toward then
        for _, ex in ipairs(list) do
            if ex.to == toward then return { x = ex.x, y = ex.y } end
        end
    end
    return { x = list[1].x, y = list[1].y }
end

-- ── Arredo via LLM su griglia ASCII (selezione + piazzamento + validatore) ───
-- L'LLM sceglie COSA e DOVE da una griglia ASCII della stanza + un menu di
-- oggetti pertinenti; il codice VALIDA e RIPARA (overlap/bordi/porte) e assegna
-- i layer. Fallback al solver deterministico se l'LLM fallisce.
--
-- ROOM_MENU: per tipo-stanza, da quali categorie/temi attingere il menu.
M.ROOM_MENU = {
    cucina   = { themes={"kitchen"},                cats={"counter","appliance"} },
    bagno    = { themes={"bathroom"},               cats={"storage","appliance"},
                 names={"toilet","sink","shower","bathtub"} },
    salotto  = { themes={"living_room"},            cats={"seating","table","electronics","plant","rug","lighting"} },
    camera   = { themes={"bedroom"},                cats={"bed","storage","rug","table","lighting"} },
    studio   = { themes={"living_room","condominium"}, cats={"table","electronics","storage","seating"} },
    ingresso = { themes={"condominium","living_room"}, cats={"storage","plant","rug"} },
}

-- ── Pilastro 2: regole oggetti (designer-in-the-loop) ────────────────────────
-- Le regole NON vivono nei 5583 oggetti del catalogo: vivono in catalog/rules.json,
-- per CATEGORIA e per NAME_CONTAINS (token nel name_it o nell'id). Il designer le
-- corregge nell'editor catalogo e il motore smette di ripetere l'errore.
-- Regole supportate per oggetto:
--   max_per_room : int   — quanti al massimo nella stessa stanza (es. wc=1, letto=1)
--   rooms        : []    — whitelist di room_type dove l'oggetto è ammesso
--                          (es. frigo→cucina, camino→salotto/studio/camera)
--   group        : str   — chiave esplicita per il conteggio max_per_room
--                          (default: categoria; o il token name_contains che ha matchato)
M._rules = { category = {}, match = {} }

--- Imposta le regole (tabella già decodificata).
function M.set_rules(r)
    if type(r) == "table" then
        M._rules = { category = r.category or {}, match = r.match or {} }
    end
end

--- Carica catalog/rules.json (path relativo alla asset-root). Silenzioso se assente.
function M.load_rules(path)
    local f = io.open(M.abs(path or "catalog/rules.json"), "r")
    if not f then return false end
    local txt = f:read("*a"); f:close()
    local ok, data = pcall(function() return require("lib/json").decode(txt) end)
    if ok and type(data) == "table" then M.set_rules(data); return true end
    return false
end

-- Risolve le regole effettive per un oggetto: merge categoria + PRIMA voce match.
-- match: { name_contains=token, ... } — token cercato in id e name_it (lowercase).
-- Ritorna { max_per_room=?, rooms=?, group=str }.
function M.object_rules(meta, id)
    local out = {}
    local cat = meta and meta.category
    if cat and M._rules.category[cat] then
        for k, v in pairs(M._rules.category[cat]) do out[k] = v end
        out.group = out.group or cat
    end
    local hay = ((id or "") .. " " .. ((meta and meta.name_it) or "")):lower()
    for _, rule in ipairs(M._rules.match or {}) do
        local tok = rule.name_contains
        if tok and hay:find(tok:lower(), 1, true) then
            for k, v in pairs(rule) do
                if k ~= "name_contains" then out[k] = v end
            end
            out.group = out.group or tok
            break
        end
    end
    return out
end

-- Applica le regole a una lista di oggetti scelti (ognuno con .id) per un dato
-- room_type: scarta i non ammessi (rooms) e i sovrannumerari (max_per_room).
-- Ordine d'arrivo = priorità (i primi vincono). Ritorna la lista filtrata.
function M.apply_rules(objects, catalog, rtype)
    if not M._rules_tried then M.load_rules(); M._rules_tried = true end
    local counts, kept = {}, {}
    for _, o in ipairs(objects) do
        local meta = catalog[o.id]
        if meta then
            local rules = M.object_rules(meta, o.id)
            local drop  = false
            if rules.rooms then
                local ok = false
                for _, rt in ipairs(rules.rooms) do if rt == rtype then ok = true; break end end
                drop = not ok
            end
            if not drop and rules.max_per_room then
                local g = rules.group or meta.category or o.id
                local n = (counts[g] or 0) + 1
                if n > rules.max_per_room then drop = true else counts[g] = n end
            end
            if not drop then kept[#kept+1] = o end
        end
    end
    return kept
end

-- categoria → layer/comportamento (riuso anche nel piazzamento LLM)
local function obj_layer(meta)
    if meta.category == "rug" then return "floor" end
    return nil   -- normale; "top" gestito da on_surface (qui non distinto)
end

-- Costruisce il menu di candidati (id, nome, footprint) per una stanza.
local function build_menu(catalog, spec, seed, cap)
    local themes = {}
    for _, t in ipairs(spec.themes or {}) do themes[t] = true end
    local cats = {}
    for _, c in ipairs(spec.cats or {}) do cats[c] = true end
    local names = spec.names
    local list = {}
    -- nomi da escludere sempre dal menu (non sono arredo da pavimento)
    local BAD_NAME = { "stair", "ladder", "scala", "elevator" }
    local function name_ok(id)
        local lid = id:lower()
        for _, b in ipairs(BAD_NAME) do if lid:find(b, 1, true) then return false end end
        return true
    end
    for id, e in pairs(catalog) do
        local ok = e.kind == "furniture" and e.complete ~= false and not e.animated
                   and e.category ~= "door" and e.category ~= "window"
                   and e.category ~= "misc" and name_ok(id)
        if ok and next(themes) and not themes[e.theme or ""] then ok = false end
        if ok and next(cats) and not cats[e.category or ""] then
            -- ammetti comunque se il nome combacia con un filtro esplicito
            ok = false
            for _, nm in ipairs(names or {}) do
                if id:lower():find(nm, 1, true) then ok = true; break end
            end
        end
        local fp = e.footprint or {1,1}
        if ok and (fp[1] > 4 or fp[2] > 3) then ok = false end   -- niente mostri
        if ok then list[#list+1] = { id=id, name=e.name_it or id, fw=fp[1], fh=fp[2],
                                      placement = e.placement or "floor",
                                      category = e.category } end
    end
    table.sort(list, function(a,b) return a.id < b.id end)
    -- campiona fino a cap, deterministico
    if #list > cap then
        local rnd = rng((seed or 1) + 7)
        local picked, used = {}, {}
        while #picked < cap do
            local i = rnd(#list)
            if not used[i] then used[i]=true; picked[#picked+1]=list[i] end
        end
        list = picked
    end
    return list
end

-- Griglia ASCII dell'interno della stanza per il prompt.
local function room_ascii(plan, r)
    local iw, ih = r.iw, r.ih
    local door_cells, win_cols = {}, {}
    for _, d in ipairs(plan.doors) do
        if d.room == r.id then
            for _, c in ipairs(d.cells) do
                local lc, lr = c[1]-r.ix, c[2]-r.iy
                if lr < 0 then lr = 0 end
                if lr >= ih then lr = ih-1 end
                door_cells[lr*1000+lc] = true
            end
        end
    end
    local lines = {}
    for row = 0, ih-1 do
        local s = {}
        for col = 0, iw-1 do
            s[#s+1] = door_cells[row*1000+col] and "D" or "."
        end
        lines[#lines+1] = table.concat(s)
    end
    return table.concat(lines, "\n")
end

-- Arreda UNA stanza via LLM. Ritorna furniture list o nil (→ fallback).
function M.furnish_llm(plan, room_id, catalog, opts)
    opts = opts or {}
    local r = plan.rooms[room_id]
    if not r then return nil end
    local rtype = r.type or ""
    local mspec = M.ROOM_MENU[rtype]
    if not mspec then return nil end

    local menu = build_menu(catalog, mspec, opts.seed, opts.menu_cap or 36)
    if #menu == 0 then return nil end
    local menu_lines = {}
    for _, m in ipairs(menu) do
        menu_lines[#menu_lines+1] = string.format("%s (%dx%d) [%s] — %s",
            m.id, m.fw, m.fh, m.placement, m.name)
    end
    local by_id = {}
    for _, m in ipairs(menu) do by_id[m.id] = m end

    local grid = room_ascii(plan, r)
    local sys = [[Sei un arredatore di interni per un RPG a tile dall'alto.
Arredi una stanza scegliendo oggetti da un MENU e posizionandoli su una GRIGLIA.
Regole:
- Usa SOLO id dal menu. Ogni oggetto occupa (larghezza x profondità) celle.
- Coordinate: riga 0 = parete NORD (in alto), ultima riga = parete SUD; col 0 = OVEST.
- (riga,col) è l'angolo in ALTO-SINISTRA dell'oggetto; deve stare dentro la griglia.
- 'D' = davanti a una porta: lascia LIBERO, non bloccare i passaggi.
- Mobili grandi (letto, divano, armadio, piano cucina) ADDOSSATI alle pareti.
- I piani cucina (counter) in FILA lungo una parete, con fornelli e lavello.
- Tappeti (rug) al centro o sotto il divano. Niente sovrapposizioni tra mobili.
- Ogni voce del menu ha un tag [posizionamento]:
  [floor]=a terra, [wall]=a parete (riga 0 o bordi), [surface]=sopra un mobile
  (mettilo sulle celle di un tavolo/piano), [rug]=tappeto, [ceiling]=al soffitto.
  Rispetta il tag: i [wall] sul muro, i [surface] su un piano, MAI in mezzo.
- Arreda in modo plausibile e ABITABILE: pochi oggetti ben disposti, non ammassati.
Rispondi SOLO con JSON.]]

    local user = string.format(
        "Stanza: %s. Griglia %dx%d (%d righe, %d colonne):\n%s\n\nMENU:\n%s\n\n"
        .. "Disponi gli oggetti adatti a questa stanza.",
        rtype, r.iw, r.ih, r.ih, r.iw, grid, table.concat(menu_lines, "\n"))

    local schema = [[{ "type":"object","required":["objects"],"properties":{
        "objects":{"type":"array","items":{"type":"object",
        "required":["id","row","col"],"properties":{
        "id":{"type":"string"},"row":{"type":"integer"},"col":{"type":"integer"}}}}}}]]

    local llm_util = require("lib/llm_util")
    local data = llm_util.validated_call(sys, user, schema, function(d)
        if type(d.objects) ~= "table" then return false, "manca l'array 'objects'" end
        return true
    end, { retries = opts.retries or 1, model = opts.model, provider = opts.provider })
    if not data then return nil end

    -- VALIDA E RIPARA: bordi, footprint, overlap, porte. Scarta gli invalidi.
    local occ = {}
    local function key(x,y) return y*1000+x end
    for _, d in ipairs(plan.doors) do
        if d.room == room_id then
            for _, c in ipairs(d.cells) do
                local lc, lr = c[1]-r.ix, c[2]-r.iy
                for _, dd in ipairs({{0,0},{0,1},{0,-1},{1,0},{-1,0}}) do
                    occ[key(lc+dd[1], lr+dd[2])] = "door"
                end
            end
        end
    end
    local function inb(col,row,fw,fh) return col>=0 and row>=0 and col+fw<=r.iw and row+fh<=r.ih end
    local function clamp(v,lo,hi) return math.max(lo, math.min(v, hi)) end
    local function free(col,row,fw,fh)
        for yy=row,row+fh-1 do for xx=col,col+fw-1 do if occ[key(xx,yy)] then return false end end end
        return true
    end
    local function take(col,row,fw,fh,id)
        for yy=row,row+fh-1 do for xx=col,col+fw-1 do occ[key(xx,yy)]=id end end
    end

    -- PILASTRO 2 — applica le regole oggetto PRIMA del piazzamento (rooms +
    -- max_per_room): scarta camino-in-cucina, due-wc, due-letti, ecc.
    local kept = M.apply_rules(data.objects, catalog, rtype)

    -- Smista per placement (campo catalogo). Ordine: floor (e piani) → wall →
    -- surface (sui piani) → rug (layer). Ogni classe ha la sua regola di riparo.
    local groups = { floor={}, wall={}, surface={}, rug={}, ceiling={} }
    for _, o in ipairs(kept) do
        local m = by_id[o.id]
        local row, col = tonumber(o.row), tonumber(o.col)
        if m and row and col then
            o._fw, o._fh, o._pl = m.fw, m.fh, m.placement or "floor"
            o._cat = m.category
            table.insert(groups[groups[o._pl] and o._pl or "floor"], o)
        end
    end

    local placed = {}
    local surfaces = {}   -- {x,y,fw,fh} dei piani (tavolo/bancone) per i surface
    -- 1) FLOOR: a terra, footprint reale, no overlap. I piani diventano surface.
    for _, o in ipairs(groups.floor) do
        local col, row = o.col, o.row
        if inb(col,row,o._fw,o._fh) and free(col,row,o._fw,o._fh) then
            take(col,row,o._fw,o._fh,o.id)
            placed[#placed+1] = { id=o.id, x=r.ix+col, y=r.iy+row, fw=o._fw, fh=o._fh }
            if o._cat=="table" or o._cat=="counter" or o._cat=="desk" then
                surfaces[#surfaces+1] = { col=col, row=row, fw=o._fw, fh=o._fh }
            end
        end
    end
    -- 2) WALL: forzati su un muro (riga 0 = nord, oppure bordi), layer top.
    for _, o in ipairs(groups.wall) do
        local col = clamp(o.col, 0, r.iw - o._fw)
        local cands = { {col,0}, {0,o.row}, {r.iw-o._fw, o.row} }
        for _, c in ipairs(cands) do
            if inb(c[1],c[2],o._fw,o._fh) and not occ[key(c[1],c[2])] then
                placed[#placed+1] = { id=o.id, x=r.ix+c[1], y=r.iy+c[2], fw=o._fw, fh=o._fh, layer="top" }
                break
            end
        end
    end
    -- 3) SURFACE: sopra un piano (centro), layer top. Niente piano → scartato.
    local si = 0
    for _, o in ipairs(groups.surface) do
        si = (#surfaces > 0) and ((si % #surfaces) + 1) or 0
        local s = surfaces[si]
        if s then
            placed[#placed+1] = { id=o.id, x=r.ix + s.col + math.floor((s.fw-o._fw)/2),
                                  y=r.iy + s.row, fw=o._fw, fh=o._fh, layer="top" }
        end
    end
    -- 4) RUG: layer pavimento (sotto), sovrapponibile, clampato.
    for _, o in ipairs(groups.rug) do
        local col = clamp(o.col, 0, r.iw-o._fw); local row = clamp(o.row, 0, r.ih-o._fh)
        placed[#placed+1] = { id=o.id, x=r.ix+col, y=r.iy+row, fw=o._fw, fh=o._fh, layer="floor" }
    end
    -- 5) CEILING: appeso (es. lampadario), layer top, posizione data.
    for _, o in ipairs(groups.ceiling) do
        local col = clamp(o.col,0,r.iw-o._fw); local row = clamp(o.row,0,r.ih-o._fh)
        placed[#placed+1] = { id=o.id, x=r.ix+col, y=r.iy+row, fw=o._fw, fh=o._fh, layer="top" }
    end
    return placed
end

-- ── Arredo dell'intera pianta + persistenza ──────────────────────────────────
-- palette = { [room_type] = { {id=...}, ... } }. Ritorna la lista piatta di
-- mobili piazzati { {id,x,y,fw,fh}, ... }, deterministica dal seed.
-- fixtures_map: { [room_type] = { run={...}, wall={...} } } — oggetti standard.
-- opts.llm = true → arreda via LLM (selezione+griglia), fallback deterministico
-- per stanza se l'LLM fallisce. opts.model/provider per il tier "gen".
function M.furnish_plan(plan, palette, catalog, seed, fixtures_map, opts)
    opts = opts or {}
    -- Ricarica le regole a ogni generazione completa: l'editor catalogo può
    -- averle riscritte tra una generazione e l'altra (designer-in-the-loop).
    M.load_rules(); M._rules_tried = true
    local furniture = {}
    for id, room in pairs(plan.rooms) do
        local got
        if opts.llm then
            got = M.furnish_llm(plan, id, catalog,
                { seed = seed, model = opts.model, provider = opts.provider,
                  retries = opts.retries })
        end
        if not got then   -- fallback deterministico (palette + fixtures)
            local items = {}
            for _, it in ipairs(palette[room.type or ""] or {}) do
                if catalog[it.id] then items[#items+1] = it end
            end
            local fix = fixtures_map and fixtures_map[room.type or ""]
            got = M.furnish(plan, id, items, catalog, seed, fix)
        end
        for _, p in ipairs(got) do furniture[#furniture+1] = p end
    end
    return furniture
end

-- Stato persistibile della scena generata: seed + spec + mobili piazzati.
-- Il salvataggio conserva l'arredo esatto anche se la palette/LLM cambia.
function M.scene_snapshot(spec, furniture, plan)
    return { seed = spec.seed, spec = spec, furniture = furniture,
             exit = plan and plan.exit }
end

-- Ricostruisce plan + furniture dallo snapshot (geometria deterministica dal
-- seed nella spec; mobili ripresi tali e quali; apertura d'uscita ripristinata).
function M.scene_restore(snap)
    if not snap or not snap.spec then return nil, nil end
    local plan = M.solve(snap.spec)
    plan.exit = snap.exit
    return plan, snap.furniture or {}
end

-- ── Config per il renderer GUI (formato get_visual_world) ────────────────────
-- Produce il config che gui_visual.h consuma: griglia inline, tileset
-- Room_Builder, stanze (con spawn interno), mobili come PNG, solid ids.
-- npcs = lista di npc già nel formato visual.npc_layered (passata così com'è).
-- stage: id del piano/cluster corrente (multi-piano). La GUI lo usa per
-- ricaricare il mondo quando il giocatore cambia stage.
function M.to_visual_config(plan, walls, catalog, furniture, npcs, stage)
    local WR = walls.wall_roles
    local wb = walls.wall_styles[plan.style] or select(2, next(walls.wall_styles))
    -- tile muro solidi (per A*): tutti i ruoli muro dello stile
    local solid = {}
    for _, role in ipairs(walls.solid_roles or {}) do
        if WR[role] then solid[#solid+1] = wb + WR[role] end
    end

    local grid = M.tile(plan, walls)
    local rows = {}
    for y = 0, plan.h - 1 do
        local row = {}
        for x = 0, plan.w - 1 do row[x+1] = grid[y][x] end
        rows[y+1] = row
    end

    -- stanze: bounds in tile + spawn al centro dell'interno
    local rdefs = {}
    for id, r in pairs(plan.rooms) do
        rdefs[#rdefs+1] = {
            name   = id,
            bounds = { r.x, r.y, r.w, r.h },
            spawn  = { r.ix + math.floor(r.iw / 2), r.iy + math.floor(r.ih / 2) },
        }
    end

    -- mobili: PNG con footprint (path assoluto per la GUI) + layer (floor/top)
    local furn = {}
    for _, p in ipairs(furniture or {}) do
        local file = p.file or (catalog[p.id] and catalog[p.id].file)
        if file then
            furn[#furn+1] = { file = M.abs(file), tile = { p.x, p.y },
                              footprint = { p.fw, p.fh }, layer = p.layer }
        end
    end

    return {
        tile_size      = 48,
        tileset        = { path = M.abs(walls.sheet), tw = 48, th = 48 },
        tile_grid      = rows,
        solid_tile_ids = solid,
        rooms          = rdefs,
        furniture      = furn,
        npcs           = npcs or {},
        stage          = stage,
        transit        = plan.transit,   -- punti di transito (scale) per stanza
    }
end

-- ── Debug: rendering ASCII della pianta ──────────────────────────────────────
function M.to_ascii(plan)
    local grid = {}
    for y = 0, plan.h - 1 do
        grid[y] = {}
        for x = 0, plan.w - 1 do grid[y][x] = " " end
    end
    -- corridoio (pavimento)
    local c = plan.corridor
    for y = c.floor_y, c.floor_y + c.h - 1 do
        for x = c.x + 1, c.x + c.w - 2 do grid[y][x] = "." end
    end
    -- stanze (muri = #, interno = lettera)
    local letter = 65
    for id, r in pairs(plan.rooms) do
        local ch = string.char(letter); letter = letter + 1
        for y = r.y, r.y + r.h - 1 do
            for x = r.x, r.x + r.w - 1 do
                if y < plan.h and x < plan.w then
                    local interior = (x >= r.ix and x < r.ix + r.iw
                                  and y >= r.iy and y < r.iy + r.ih)
                    grid[y][x] = interior and string.lower(ch) or "#"
                end
            end
        end
    end
    -- porte
    for _, d in ipairs(plan.doors) do
        for _, cell in ipairs(d.cells) do
            if grid[cell[2]] then grid[cell[2]][cell[1]] = "+" end
        end
    end
    local lines = {}
    for y = 0, plan.h - 1 do
        local row = {}
        for x = 0, plan.w - 1 do row[x+1] = grid[y][x] end
        lines[#lines+1] = table.concat(row)
    end
    return table.concat(lines, "\n")
end

-- ── Self-test ─────────────────────────────────────────────────────────────────
if arg and arg[0] and arg[0]:match("visual_gen%.lua$") then
    local spec = {
        seed = M.hash("apt_203"),
        entrance = "ingresso",
        style = "grigio_chiaro",
        rooms = {
            { id = "ingresso", type = "ingresso" },
            { id = "salotto",  type = "salotto" },
            { id = "cucina",   type = "cucina" },
            { id = "bagno",    type = "bagno" },
            { id = "camera",   type = "camera" },
        },
        edges = {
            { "ingresso", "salotto" }, { "salotto", "cucina" },
            { "salotto", "camera" }, { "cucina", "bagno" },
        },
    }
    local plan = M.solve(spec)
    print("pianta " .. plan.w .. "x" .. plan.h .. " — stanze: " ..
          (function() local n=0; for _ in pairs(plan.rooms) do n=n+1 end; return n end)())
    local issues = M.validate_plan(plan)
    print(#issues == 0 and "valida ✓" or ("PROBLEMI: " .. table.concat(issues, "; ")))
    print(M.to_ascii(plan))
    -- dump tiler+arredo JSON se richiesto (luajit ... --tile out.json)
    if arg[1] == "--tile" and arg[2] then
        package.path = "scripts/?.lua;scripts/lib/?.lua;" .. package.path
        local json  = require("lib/json")
        local function load_json(p) local f=io.open(p,"r"); local d=json.decode(f:read("*a")); f:close(); return d end
        local walls   = load_json("catalog/walls.json")
        local catalog = load_json("catalog/objects.json")
        -- palette di arredo per tipo (placeholder: in produzione la sceglie l'LLM)
        local PALETTE = {
            salotto = { {id="sofa_blue_3seat",count=1}, {id="armchair_blue",count=1},
                        {id="tv_flat_screen",count=1}, {id="decor_potted_plant_small",count=1},
                        {id="console_table_wood",count=1} },
            cucina  = { {id="kitchen_stove",count=1}, {id="cabinet_kitchen_corner",count=1},
                        {id="display_cooler_double_door",count=1} },
            camera  = { {id="bed_brown_single",count=1}, {id="bedside_table_wood",count=1},
                        {id="bathroom_cabinet_white_2",count=1} },
            bagno   = { {id="bathroom_toilet_gray",count=1}, {id="bathroom_sink_counter_green",count=1} },
            ingresso= { {id="clothes_rack_empty",count=1} },
        }
        local furniture = {}
        for id, room in pairs(plan.rooms) do
            local items = PALETTE[room.type or ""] or {}
            -- tieni solo gli id realmente esistenti nel catalogo
            local valid = {}
            for _, it in ipairs(items) do if catalog[it.id] then valid[#valid+1]=it end end
            for _, p in ipairs(M.furnish(plan, id, valid, catalog, spec.seed)) do
                p.file = catalog[p.id] and catalog[p.id].file
                furniture[#furniture+1] = p
            end
        end
        local out = io.open(arg[2], "w")
        out:write(M.tile_json(plan, walls, furniture)); out:close()
        print("tiler+arredo → " .. arg[2] .. " (" .. #furniture .. " oggetti)")
    end
end

return M
