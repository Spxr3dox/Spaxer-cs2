client.register_callback("paint", function()
    local w, h = render.screen_size()
    local fps = engine.get_fps()
    local ping = engine.get_ping()
    local in_game = engine.is_in_game()
    local ping_str = ping >= 0 and (tostring(ping) .. " ms") or "N/A"
    local state_str = in_game and "IN-GAME" or "LOBBY"
    local text = string.format("SPAXER.LUA | %d FPS | %s | %s", fps, ping_str, state_str)

    local tw, th = render.measure_text(text, 12, true)
    local pad_x, pad_y = 12, 6
    local box_w, box_h = tw + pad_x * 2, th + pad_y * 2
    local box_x, box_y = w - box_w - 20, 20

    render.rect_filled(box_x, box_y, box_w, box_h, 18, 18, 24, 210, 6)
    render.rect(box_x, box_y, box_w, box_h, 255, 255, 255, 40, 1.0, 6)
    render.rect_filled(box_x, box_y, box_w, 2, 0, 122, 255, 255, 2)
    render.text(box_x + pad_x, box_y + pad_y, text, 240, 240, 245, 255, 12, true)
end)
