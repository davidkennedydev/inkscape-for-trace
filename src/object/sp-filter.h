// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * SVG <filter> element
 *//*
 * Authors:
 *   Hugo Rodrigues <haa.rodrigues@gmail.com>
 *   Niko Kiirala <niko@kiirala.com>
 *
 * Copyright (C) 2006,2007 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SP_FILTER_H_SEEN
#define SP_FILTER_H_SEEN

#include <map>
#include <memory>
#include <glibmm/ustring.h>

#include "number-opt-number.h"
#include "sp-dimensions.h"
#include "sp-filter-units.h"
#include "sp-item.h"
#include "sp-object.h"
#include "svg/svg-length.h"

#define SP_FILTER_FILTER_UNITS(f) (SP_FILTER(f)->filterUnits)
#define SP_FILTER_PRIMITIVE_UNITS(f) (SP_FILTER(f)->primitiveUnits)

namespace Inkscape {
class Drawing;
class DrawingItem;
namespace Filters {
class Filter;
} // namespace Filters
} // namespace Inkscape

class SPFilterReference;
class SPFilterPrimitive;

class SPFilter
    : public SPObject
    , public SPDimensions
{
public:
    SPFilter();
    ~SPFilter() override;

    // Returns a renderer for this filter, for use by the DrawingItem item.
    std::unique_ptr<Inkscape::Filters::Filter> build_renderer(Inkscape::DrawingItem *item) const;

    /// Returns the number of filter primitives in this SPFilter object.
    int primitive_count() const;

    /// Returns a slot number for given image name, or -1 for unknown name.
    int get_image_name(char const *name) const;

    /// Returns slot number for given image name, even if it's unknown.
    int set_image_name(char const *name);

    void update_filter_all_regions();
    void update_filter_region(SPItem *item);
    void set_filter_region(double x, double y, double width, double height);
    Geom::Rect get_automatic_filter_region(SPItem *item);

    // Checks each filter primitive to make sure the object won't cause issues
    bool valid_for(SPObject const *obj) const;

    /** Finds image name based on its slot number. Returns 0 for unknown slot
     * numbers. */
    char const *name_for_image(int image) const;

    /// Returns a result image name that is not in use inside this filter.
    Glib::ustring get_new_result_name() const;

    void show(Inkscape::DrawingItem *item);
    void hide(Inkscape::DrawingItem *item);

    SPFilterUnits filterUnits;
    unsigned int filterUnits_set : 1;
    SPFilterUnits primitiveUnits;
    unsigned int primitiveUnits_set : 1;
    NumberOptNumber filterRes;
    std::unique_ptr<SPFilterReference> href;
    bool auto_region;

    sigc::connection modified_connection;

    unsigned getRefCount();
    unsigned _refcount;

protected:
    void build(SPDocument *doc, Inkscape::XML::Node *repr) override;
    void release() override;

    void child_added(Inkscape::XML::Node *child, Inkscape::XML::Node *ref) override;
    void remove_child(Inkscape::XML::Node *child) override;
    void order_changed(Inkscape::XML::Node* child, Inkscape::XML::Node* old_repr, Inkscape::XML::Node* new_repr) override;

    void set(SPAttr key, char const *value) override;

    void modified(unsigned flags) override;
    void update(SPCtx *ctx, unsigned flags) override;

    Inkscape::XML::Node *write(Inkscape::XML::Document *doc, Inkscape::XML::Node *repr, unsigned flags) override;

private:
    std::map<std::string, int> _image_name;
    int _image_number_next;

    std::vector<Inkscape::DrawingItem*> views;
};

MAKE_SP_OBJECT_DOWNCAST_FUNCTIONS(SP_FILTER, SPFilter)
MAKE_SP_OBJECT_TYPECHECK_FUNCTIONS(SP_IS_FILTER, SPFilter)

#endif /* !SP_FILTER_H_SEEN */

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
