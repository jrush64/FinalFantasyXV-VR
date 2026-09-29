#pragma once
#include <array>
#include <cstddef>
// Same-frame two-phase submission. Completion must release only owned resources.
// Copy returns true only when it queued GPU work; all such work is flushed before
// any destination image or cross-device ownership is released.
template<std::size_t N, class Stage, class Copy, class Flush, class Complete>
void RunTransferBatch(Stage stage, Copy copy, Flush flush, Complete complete)
{
    std::array<bool, N> staged{};
    for (std::size_t i=0;i<N;++i) staged[i]=stage(i);
    bool dirty=false;
    for (std::size_t i=0;i<N;++i) if(staged[i]) dirty=copy(i)||dirty;
    if(dirty) flush();
    for (std::size_t i=0;i<N;++i) if(staged[i]) complete(i);
}
