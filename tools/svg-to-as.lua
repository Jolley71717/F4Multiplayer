-- Writes ui/shared/f4mp/ui/LogoArt.as from ui/shared/f4mp/ui/logo.svg: the logo's outlines as points for Flash to fill.
-- Run it when the logo changes: xmake lua tools/svg-to-as.lua
--
-- The movies are compiled without Flex's framework, so an [Embed] of an SVG won't build. This reads one fill's worth of paths using only
-- M, m, l, c and z, which is what the logo has, and flattens each curve to short lines.

local STEPS = 8 -- lines to a curve

local function outlines(data, shapes)
    local tokens = {}
    local at = 1
    while at <= #data do
        local letter = data:match("^%s*,?%s*([A-Za-z])", at)
        if letter then
            tokens[#tokens + 1] = letter
            local _, to = data:find("^%s*,?%s*[A-Za-z]", at)
            at = to + 1
        else
            local from, to = data:find("^%s*,?%s*-?%d*%.?%d+[eE]?[-+]?%d*", at)
            if not from then
                break
            end
            tokens[#tokens + 1] = tonumber(data:sub(from, to):match("-?[%d%.]+[eE]?[-+]?%d*"))
            at = to + 1
        end
    end

    local index, x, y, command = 1, 0, 0, ""
    local function number()
        index = index + 1
        return tokens[index - 1]
    end
    while index <= #tokens do
        if type(tokens[index]) == "string" then
            command = tokens[index]
            index = index + 1
        end
        if command == "z" or command == "Z" then
            -- closed when it's drawn
        elseif command == "M" or command == "m" then
            local nx, ny = number(), number()
            if command == "M" then
                x, y = nx, ny
            else
                x, y = x + nx, y + ny
            end
            shapes[#shapes + 1] = { { x, y } }
            command = "l"
        elseif command == "l" then
            x = x + number()
            y = y + number()
            local shape = shapes[#shapes]
            shape[#shape + 1] = { x, y }
        elseif command == "c" then
            local c1x, c1y, c2x, c2y, ex, ey = number(), number(), number(), number(), number(), number()
            local shape = shapes[#shapes]
            for step = 1, STEPS do
                local t = step / STEPS
                local u = 1 - t
                shape[#shape + 1] = {
                    x + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * ex,
                    y + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * ey,
                }
            end
            x, y = x + ex, y + ey
        else
            raise("logo.svg: no support for path command " .. tostring(command))
        end
    end
end

function main()
    local folder = path.join(os.projectdir(), "ui", "shared", "f4mp", "ui")
    local svg = io.readfile(path.join(folder, "logo.svg"))
    local left, top, width, height = svg:match('viewBox="(%S+) (%S+) (%S+) (%S+)"')
    left, top, width, height = tonumber(left), tonumber(top), tonumber(width), tonumber(height)

    local shapes = {}
    for data in svg:gmatch('%sd="([^"]+)"') do
        outlines(data, shapes)
    end

    local rows = {}
    for _, shape in ipairs(shapes) do
        local points = {}
        for _, point in ipairs(shape) do
            points[#points + 1] = string.format("%.1f, %.1f", point[1] - left, point[2] - top)
        end
        rows[#rows + 1] = "\t\t\t[" .. table.concat(points, ", ") .. "]"
    end
    io.writefile(path.join(folder, "LogoArt.as"), "package f4mp.ui\n{\n"
        .. "\t// Written by tools/svg-to-as.lua from logo.svg: don't edit. Each outline is x, y, x, y..., in logo.svg's units.\n"
        .. "\tinternal final class LogoArt\n\t{\n"
        .. string.format("\t\tpublic static const WIDTH:Number = %.1f;\n", width)
        .. string.format("\t\tpublic static const HEIGHT:Number = %.1f;\n", height)
        .. "\t\tpublic static const OUTLINES:Array = [\n" .. table.concat(rows, ",\n") .. "\n\t\t];\n\t}\n}\n")
end
