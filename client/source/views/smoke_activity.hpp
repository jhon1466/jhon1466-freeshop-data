#pragma once

#include <borealis.hpp>

namespace freeshop
{

// A deliberately minimal first screen, built only to answer the questions
// that can't be answered from a successful compile: does Borealis actually
// boot on the console, does the pipensx theme reach the widgets, and does
// controller input reach the framework. Replaced by the real TabFrame
// shell once those are confirmed.
class SmokeActivity : public brls::Activity
{
  public:
    brls::View* createContentView() override;
};

} // namespace freeshop
