client.register_callback("paint", function()
    local bomb = entities.get_bomb()
    if not bomb.visible then return end

    local w, h = render.screen_size()
    local text = string.format("C4 SITE %s: %.1fs", bomb.site == 1 and "B" or "A", bomb.blow_secs)
    if bomb.being_defused then
        text = text .. string.format(" | DEFUSING: %.1fs", bomb.defuse_secs)
    end

    local tw, th = render.measure_text(text, 13, true)
    local cx = w / 2 - (tw + 24) / 2
    local cy = 40

    render.rect_filled(cx, cy, tw + 24, 28, 20, 20, 25, 220, 6)
    local r, g, b = 255, 69, 58
    if bomb.blow_secs > 10 then
        r, g, b = 52, 199, 89
    elseif bomb.blow_secs > 5 then
        r, g, b = 255, 159, 10
    end
    render.rect_filled(cx, cy, tw + 24, 2, r, g, b, 255, 2)
    render.text(cx + 12, cy + 6, text, 255, 255, 255, 255, 13, true)
end)
