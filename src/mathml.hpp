// mathml.hpp — LaTeX -> MathML Core converter (build-time, no JS needed in the browser).
#pragma once
#include <string>

// Convert a LaTeX math fragment to a <math> element.
// display=true renders block math, false renders inline.
std::string latex_to_mathml(const std::string &tex, bool display);
