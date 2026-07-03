#include <doctest/doctest.h>
#include "core/PanelSlot.h"

using namespace oss;

static Port mk(PortType t) { Port p; p.type = t; return p; }

TEST_CASE("inputSlot routes control inputs to Controls, fields to Properties") {
    CHECK(inputSlot(mk(PortType::Float)) == PanelSlot::Controls);           // plain slider

    Port choice = mk(PortType::Float); choice.choices = {"a", "b"};
    CHECK(inputSlot(choice) == PanelSlot::Properties);                       // dropdown
    Port intp = mk(PortType::Float); intp.integer = true;
    CHECK(inputSlot(intp) == PanelSlot::Properties);                         // integer field

    CHECK(inputSlot(mk(PortType::Bool))   == PanelSlot::Properties);
    CHECK(inputSlot(mk(PortType::Colour)) == PanelSlot::Properties);
    CHECK(inputSlot(mk(PortType::String)) == PanelSlot::Properties);

    CHECK(inputSlot(mk(PortType::Texture)) == PanelSlot::None);
    CHECK(inputSlot(mk(PortType::Audio))   == PanelSlot::None);
}
