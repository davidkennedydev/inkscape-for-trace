// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief SVG blend filter effect
 *//*
 * Authors:
 *   Hugo Rodrigues <haa.rodrigues@gmail.com>
 *   Niko Kiirala <niko@kiirala.com>
 *
 * Copyright (C) 2006,2007 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SP_FEBLEND_H_SEEN
#define SP_FEBLEND_H_SEEN

#include "sp-filter-primitive.h"
#include "display/nr-filter-blend.h"

class SPFeBlend
    : public SPFilterPrimitive
{
public:
    SPBlendMode get_blend_mode() const { return blend_mode; }
    int get_in2() const { return in2; }

private:
    SPBlendMode blend_mode = SP_CSS_BLEND_NORMAL;
    int in2 = Inkscape::Filters::NR_FILTER_SLOT_NOT_SET;

protected:
    void build(SPDocument *doc, Inkscape::XML::Node *repr) override;
    void set(SPAttr key, char const *value) override;
    Inkscape::XML::Node *write(Inkscape::XML::Document *doc, Inkscape::XML::Node *repr, unsigned flags) override;

    std::unique_ptr<Inkscape::Filters::FilterPrimitive> build_renderer() const override;
};

MAKE_SP_OBJECT_DOWNCAST_FUNCTIONS(SP_FEBLEND, SPFeBlend)
MAKE_SP_OBJECT_TYPECHECK_FUNCTIONS(SP_IS_FEBLEND, SPFeBlend)

#endif // SP_FEBLEND_H_SEEN

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
