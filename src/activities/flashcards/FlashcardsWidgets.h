#pragma once

#include <FlashcardTypes.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>

#include "components/themes/BaseTheme.h"

namespace flashcards_ui {

enum class InkTexture : uint8_t {
  Solid,
  Dither50,
  Paper,
};

inline void fillTexturedRect(const GfxRenderer& renderer, const Rect& rect, const InkTexture texture) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  switch (texture) {
    case InkTexture::Paper:
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
      break;
    case InkTexture::Solid:
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
      break;
    case InkTexture::Dither50: {
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
      const int xEnd = rect.x + rect.width;
      const int yEnd = rect.y + rect.height;
      for (int py = rect.y; py < yEnd; ++py) {
        for (int px = rect.x; px < xEnd; ++px) {
          if (((static_cast<uint32_t>(px) + static_cast<uint32_t>(py)) & 1u) == 0u) {
            renderer.drawPixel(px, py, true);
          }
        }
      }
      break;
    }
  }
}

inline void drawSessionRibbon(const GfxRenderer& renderer, const Rect& rect,
                              const flashcards::SessionProgress& progress, const bool forceFullSolid = false) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  if (forceFullSolid) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
    return;
  }

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const uint32_t total = static_cast<uint32_t>(progress.cleared) + static_cast<uint32_t>(progress.drilling) +
                         static_cast<uint32_t>(progress.unseen);
  if (total > 0) {
    int endCleared = 0;
    if (progress.cleared > 0) {
      if (progress.drilling == 0 && progress.unseen == 0) {
        endCleared = rect.width;
      } else {
        const int reservedAfter = (progress.drilling > 0 ? 1 : 0) + (progress.unseen > 0 ? 1 : 0);
        const int maxCleared = std::max(1, rect.width - reservedAfter);
        const int rawCleared =
            static_cast<int>((static_cast<uint64_t>(progress.cleared) * static_cast<uint64_t>(rect.width)) / total);
        endCleared = std::clamp(rawCleared, 1, maxCleared);
      }
    }

    int endActive = endCleared;
    if (progress.drilling > 0) {
      if (progress.unseen == 0) {
        endActive = rect.width;
      } else {
        const int minActive = std::min(rect.width, endCleared + 1);
        const int maxActive = std::max(minActive, rect.width - 1);
        const int rawActive = static_cast<int>(
            (static_cast<uint64_t>(progress.cleared + progress.drilling) * static_cast<uint64_t>(rect.width)) / total);
        endActive = std::clamp(rawActive, minActive, maxActive);
      }
    }

    if (endCleared > 0) {
      renderer.fillRect(rect.x, rect.y, endCleared, rect.height, true);
    }
    if (endActive > endCleared) {
      fillTexturedRect(renderer, Rect(rect.x + endCleared, rect.y, endActive - endCleared, rect.height),
                       InkTexture::Dither50);
      if (progress.unseen > 0 && endActive < rect.width) {
        const int sepX = rect.x + endActive - 1;
        renderer.drawLine(sepX, rect.y, sepX, rect.y + rect.height - 1, true);
      }
    }
  }

  if (rect.height >= 3) {
    renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);
  }
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

inline void drawPassScoreBar(const GfxRenderer& renderer, const Rect& rect, const uint32_t firstTryCount,
                             const uint32_t retriedCount, const int cornerRadius = 5) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const int innerX = rect.x + 1;
  const int innerY = rect.y + 1;
  const int innerW = rect.width - 2;
  const int innerH = rect.height - 2;

  const uint64_t total = static_cast<uint64_t>(firstTryCount) + static_cast<uint64_t>(retriedCount);
  if (innerW > 0 && innerH > 0 && total > 0) {
    if (retriedCount == 0) {
      fillTexturedRect(renderer, Rect(innerX, innerY, innerW, innerH), InkTexture::Solid);
    } else if (firstTryCount == 0) {
      fillTexturedRect(renderer, Rect(innerX, innerY, innerW, innerH), InkTexture::Dither50);
    } else if (innerW == 1) {
      fillTexturedRect(renderer, Rect(innerX, innerY, 1, innerH),
                       firstTryCount >= retriedCount ? InkTexture::Solid : InkTexture::Dither50);
    } else if (innerW == 2) {
      fillTexturedRect(renderer, Rect(innerX, innerY, 1, innerH), InkTexture::Solid);
      fillTexturedRect(renderer, Rect(innerX + 1, innerY, 1, innerH), InkTexture::Dither50);
    } else {
      const int fillW = innerW - 1;
      const int rawSolid =
          static_cast<int>((static_cast<uint64_t>(firstTryCount) * static_cast<uint64_t>(fillW) + total / 2u) / total);
      const int solidW = std::clamp(rawSolid, 1, fillW - 1);
      const int divX = innerX + solidW;
      const int retriedX = divX + 1;
      const int retriedW = innerX + innerW - retriedX;
      fillTexturedRect(renderer, Rect(innerX, innerY, solidW, innerH), InkTexture::Solid);
      renderer.drawLine(divX, innerY, divX, innerY + innerH - 1, true);
      fillTexturedRect(renderer, Rect(retriedX, innerY, retriedW, innerH), InkTexture::Dither50);
    }
  }

  if (cornerRadius <= 1 || rect.height <= 6) {
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  } else {
    const int maxRadius = std::min({cornerRadius, rect.width / 2, rect.height / 2});
    renderer.maskRoundedRectOutsideCorners(rect.x, rect.y, rect.width, rect.height, maxRadius + 1, Color::White);
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, maxRadius, true);
  }
}

inline void drawSwatch(const GfxRenderer& renderer, const Rect& rect, const InkTexture texture) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }
  if (rect.width > 2 && rect.height > 2) {
    fillTexturedRect(renderer, Rect(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2), texture);
  } else {
    fillTexturedRect(renderer, rect, texture);
  }
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
}

}  // namespace flashcards_ui
