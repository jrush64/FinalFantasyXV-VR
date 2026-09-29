#pragma once

struct IDXGISwapChain;

namespace AerMenu
{
// Called on the game's Present thread. Insert toggles the menu.
// Initialization is fail-open: AER keeps running if the overlay cannot start.
void OnPresent(IDXGISwapChain* swapChain);
}
