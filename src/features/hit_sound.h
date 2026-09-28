#pragma once

namespace hitsound {

enum class Style : int { Off = 0, Click = 1, Ding = 2, Bell = 3, Pop = 4 };

void Play(Style style, int volume_percent, bool kill);

}
