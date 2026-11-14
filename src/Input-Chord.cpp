#include "Input.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sdl3mcp {
namespace {

constexpr char kChordSeparator = '+';

/* Lock keys (caps/num/scroll) and AltGr cannot be synthesized, so reject them. */
constexpr SDL_Keymod kSupportedModifiers =
    SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI;

bool isAsciiWhitespace(char character) noexcept {
   return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' || character == '\v';
}

std::string_view trimWhitespace(std::string_view text) noexcept {
   std::size_t begin = 0;
   std::size_t end = text.size();
   while (begin < end && isAsciiWhitespace(text[begin])) {
      ++begin;
   }
   while (end > begin && isAsciiWhitespace(text[end - 1])) {
      --end;
   }
   return text.substr(begin, end - begin);
}

/*
  Layout lookups report required modifiers as the generic CTRL/SHIFT/ALT/GUI
  bits. Replay owns physical keys, so fold them onto the left side: '!' becomes
  LSHIFT rather than "either shift".
*/
Result<SDL_Keymod, ProtocolError> leftSideModifiers(SDL_Keymod required) {
   const auto unsupported = static_cast<SDL_Keymod>(required & ~kSupportedModifiers);
   if (unsupported != SDL_KMOD_NONE) {
      return ProtocolError{"key requires an unsupported modifier"};
   }

   SDL_Keymod leftSide = SDL_KMOD_NONE;
   if ((required & SDL_KMOD_CTRL) != 0) {
      leftSide |= SDL_KMOD_LCTRL;
   }
   if ((required & SDL_KMOD_SHIFT) != 0) {
      leftSide |= SDL_KMOD_LSHIFT;
   }
   if ((required & SDL_KMOD_ALT) != 0) {
      leftSide |= SDL_KMOD_LALT;
   }
   if ((required & SDL_KMOD_GUI) != 0) {
      leftSide |= SDL_KMOD_LGUI;
   }
   return leftSide;
}

/* `modifiers` is already left-side only; the binding adds what the key needs. */
Result<KeyChord, ProtocolError> toChord(const KeyBinding& binding,
                                        SDL_Keymod modifiers) {
   const Result<SDL_Keymod, ProtocolError> required =
       leftSideModifiers(binding.modifiers);
   if (!required) {
      return required.error();
   }
   return KeyChord{binding.scancode,
                   static_cast<SDL_Keymod>(*required | modifiers)};
}

SDL_Keymod modifierBits(std::span<const Modifier> modifiers) noexcept {
   SDL_Keymod bits = SDL_KMOD_NONE;
   for (const Modifier modifier : modifiers) {
      bits |= modifierBit(modifier);
   }
   return bits;
}

/*
  Splits the spec at '+'. Every segment but the last must name a modifier; the
  last is the key. Repeated bits merge idempotently because the mask is an OR.
*/
Result<KeyChord, ProtocolError> resolveChord(std::string_view spec,
                                             SDL_Keymod modifiers,
                                             const KeyMap& keymap) {
   std::string_view keySegment;
   std::size_t begin = 0;
   for (;;) {
      const std::size_t separator = spec.find(kChordSeparator, begin);
      const std::size_t length =
          separator == std::string_view::npos ? std::string_view::npos : separator - begin;
      const std::string_view segment = trimWhitespace(spec.substr(begin, length));
      if (segment.empty()) {
         return ProtocolError{"empty segment in key chord \"" +
                              std::string(spec) + "\""};
      }

      if (separator == std::string_view::npos) {
         keySegment = segment;
         break;
      }

      const std::optional<Modifier> modifier = parseModifier(segment);
      if (!modifier.has_value()) {
         return ProtocolError{"unknown modifier \"" + std::string(segment) + "\""};
      }
      modifiers |= modifierBit(*modifier);
      begin = separator + 1;
   }

   const std::optional<KeyBinding> binding = keymap.find(keySegment);
   if (!binding.has_value()) {
      return ProtocolError{"unknown key \"" + std::string(keySegment) + "\""};
   }
   return toChord(*binding, modifiers);
}

}  // namespace

Result<KeyChord, ProtocolError> resolveKey(std::string_view spec,
                                           const KeyMap& keymap) {
   const std::string_view trimmed = trimWhitespace(spec);
   if (trimmed.empty()) {
      return ProtocolError{"key name is empty"};
   }

   /* The whole name first: "keypad +" is a key, not a malformed chord. */
   const std::optional<KeyBinding> binding = keymap.find(trimmed);
   if (binding.has_value()) {
      return toChord(*binding, SDL_KMOD_NONE);
   }

   if (trimmed.find(kChordSeparator) != std::string_view::npos) {
      return ProtocolError{"key \"" + std::string(trimmed) +
                           "\" is a chord; chords are only valid for press"};
   }
   return ProtocolError{"unknown key \"" + std::string(trimmed) + "\""};
}

Result<KeyChord, ProtocolError> resolveKeyChord(
    std::string_view spec,
    std::span<const Modifier> explicitModifiers,
    const KeyMap& keymap) {
   const std::string_view trimmed = trimWhitespace(spec);
   if (trimmed.empty()) {
      return ProtocolError{"key name is empty"};
   }

   const SDL_Keymod modifiers = modifierBits(explicitModifiers);

   /* Keep names that literally contain '+' ("keypad +") out of the splitter. */
   const std::optional<KeyBinding> whole = keymap.find(trimmed);
   if (whole.has_value()) {
      return toChord(*whole, modifiers);
   }
   return resolveChord(trimmed, modifiers, keymap);
}

}  // namespace sdl3mcp
