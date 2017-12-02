/** \file
 * Pencil event context implementation.
 */

/*
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2000 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 * Copyright (C) 2002 Lauris Kaplinski
 * Copyright (C) 2004 Monash University
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include <gdk/gdkkeysyms.h>

#include "ui/tools/pencil-tool.h"
#include "desktop.h"
#include "inkscape.h"

#include "selection.h"
#include "selection-chemistry.h"
#include "ui/draw-anchor.h"
#include "message-stack.h"
#include "message-context.h"
#include "sp-path.h"
#include "snap.h"
#include "pixmaps/cursor-pencil.xpm"
#include <2geom/sbasis-to-bezier.h>
#include <2geom/bezier-utils.h>
#include "display/canvas-bpath.h"
#include <glibmm/i18n.h>
#include "context-fns.h"
#include "sp-namedview.h"
#include "xml/node.h"
#include "xml/sp-css-attr.h"
#include "svg/svg.h"
#include "display/curve.h"
#include "desktop-style.h"
#include "style.h"
#include "display/sp-canvas.h"
#include "display/curve.h"
#include "live_effects/lpe-powerstroke.h"
#include "ui/tool/event-utils.h"

namespace Inkscape {
namespace UI {
namespace Tools {

static Geom::Point pencil_drag_origin_w(0, 0);
static bool pencil_within_tolerance = false;

static bool in_svg_plane(Geom::Point const &p) { return Geom::LInfty(p) < 1e18; }
const double HANDLE_CUBIC_GAP = 0.01;

const std::string& PencilTool::getPrefsPath() {
	return PencilTool::prefsPath;
}

const std::string PencilTool::prefsPath = "/tools/freehand/pencil";

PencilTool::PencilTool()
    : FreehandBase(cursor_pencil_xpm)
    , p()
    , npoints(0)
    , state(SP_PENCIL_CONTEXT_IDLE)
    , req_tangent(0, 0)
    , is_drawing(false)
    , points_parsed(0)
    , sketch_n(0)
    , _powerpreviewtail(NULL)
    , _powerpreview(NULL)
    , _preview_ok(true)
{
}

void PencilTool::setup() {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (prefs->getBool("/tools/freehand/pencil/selcue")) {
        this->enableSelectionCue();
    }

    FreehandBase::setup();

    this->is_drawing = false;
    this->anchor_statusbar = false;
}



PencilTool::~PencilTool() {
}

void PencilTool::_extinput(GdkEvent *event) {
    if (gdk_event_get_axis (event, GDK_AXIS_PRESSURE, &this->pressure)) {
        this->pressure = CLAMP (this->pressure, DDC_MIN_PRESSURE, DDC_MAX_PRESSURE);
    } else {
        this->pressure = DDC_DEFAULT_PRESSURE;
        //If no pressure device ignore pressure button
        input_has_pressure = false;
    }
}

/** Snaps new node relative to the previous node. */
void PencilTool::_endpointSnap(Geom::Point &p, guint const state) {
    if ((state & GDK_CONTROL_MASK)) { //CTRL enables constrained snapping
        if (this->npoints > 0) {
            spdc_endpoint_snap_rotation(this, p, this->p[0], state);
        }
    } else {
        if (!(state & GDK_SHIFT_MASK)) { //SHIFT disables all snapping, except the angular snapping above
                                         //After all, the user explicitely asked for angular snapping by
                                         //pressing CTRL
            boost::optional<Geom::Point> origin = this->npoints > 0 ? this->p[0] : boost::optional<Geom::Point>();
            spdc_endpoint_snap_free(this, p, origin, state);
        }
    }
}

/**
 * Callback for handling all pencil context events.
 */
bool PencilTool::root_handler(GdkEvent* event) {
    bool ret = false;
    this->_extinput(event);
    switch (event->type) {
        case GDK_BUTTON_PRESS:
            ret = this->_handleButtonPress(event->button);
            break;

        case GDK_MOTION_NOTIFY:
            ret = this->_handleMotionNotify(event->motion);
            break;

        case GDK_BUTTON_RELEASE:
            ret = this->_handleButtonRelease(event->button);
            break;

        case GDK_KEY_PRESS:
            ret = this->_handleKeyPress(event->key);
            break;

        case GDK_KEY_RELEASE:
            ret = this->_handleKeyRelease(event->key);
            break;

        default:
            break;
    }
    if (!ret) {
        ret = FreehandBase::root_handler(event);
    }

    return ret;
}

bool PencilTool::_handleButtonPress(GdkEventButton const &bevent) {
    bool ret = false;
    if ( bevent.button == 1  && !this->space_panning) {
        Inkscape::Selection *selection = desktop->getSelection();

        if (Inkscape::have_viable_layer(desktop, this->message_context) == false) {
            return true;
        }

        if (!this->grab) {
            /* Grab mouse, so release will not pass unnoticed */
            this->grab = SP_CANVAS_ITEM(desktop->acetate);
            sp_canvas_item_grab(this->grab, ( GDK_KEY_PRESS_MASK | GDK_BUTTON_PRESS_MASK   |
                                            GDK_BUTTON_RELEASE_MASK |
                                            GDK_POINTER_MOTION_MASK  ),
                                NULL, bevent.time);
        }

        Geom::Point const button_w(bevent.x, bevent.y);

        /* Find desktop coordinates */
        Geom::Point p = this->desktop->w2d(button_w);

        /* Test whether we hit any anchor. */
        SPDrawAnchor *anchor = spdc_test_inside(this, button_w);

        pencil_drag_origin_w = Geom::Point(bevent.x,bevent.y);
        pencil_within_tolerance = true;
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        input_has_pressure = prefs->getBool("/tools/freehand/pencil/pressure", true);
        if (input_has_pressure) {
            this->state = SP_PENCIL_CONTEXT_FREEHAND;
        }
        switch (this->state) {
            case SP_PENCIL_CONTEXT_ADDLINE:
                /* Current segment will be finished with release */
                ret = true;
                break;
            default:
                /* Set first point of sequence */
                SnapManager &m = desktop->namedview->snap_manager;
                if (bevent.state & GDK_CONTROL_MASK) {
                    m.setup(desktop, true);
                    if (!(bevent.state & GDK_SHIFT_MASK)) {
                        m.freeSnapReturnByRef(p, Inkscape::SNAPSOURCE_NODE_HANDLE);
                      }
                    spdc_create_single_dot(this, p, "/tools/freehand/pencil", bevent.state);
                    m.unSetup();
                    ret = true;
                    break;
                }
                if (anchor) {
                    p = anchor->dp;
                    this->overwrite_curve = anchor->curve;
                    desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Continuing selected path"));
                } else {
                    m.setup(desktop, true);
                    if (!(bevent.state & GDK_SHIFT_MASK)) {
                        // This is the first click of a new curve; deselect item so that
                        // this curve is not combined with it (unless it is drawn from its
                        // anchor, which is handled by the sibling branch above)
                        selection->clear();
                        desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Creating new path"));
                        if (!input_has_pressure) {
                            m.freeSnapReturnByRef(p, Inkscape::SNAPSOURCE_NODE_HANDLE);
                        }
                    } else if (selection->singleItem() && SP_IS_PATH(selection->singleItem())) {
                        desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Appending to selected path"));
                        m.freeSnapReturnByRef(p, Inkscape::SNAPSOURCE_NODE_HANDLE);
                    }
                    m.unSetup();
                }
                this->sa = anchor;
                this->_setStartpoint(p);
                ret = true;
                break;
        }

        this->is_drawing = true;
    }
    return ret;
}

bool PencilTool::_handleMotionNotify(GdkEventMotion const &mevent) {
    if ((mevent.state & GDK_CONTROL_MASK) && (mevent.state & GDK_BUTTON1_MASK)) {
        // mouse was accidentally moved during Ctrl+click;
        // ignore the motion and create a single point
        this->is_drawing = false;
        return true;
    }

    bool ret = false;
    
    if (this->space_panning || (mevent.state & GDK_BUTTON2_MASK) || (mevent.state & GDK_BUTTON3_MASK)) {
        // allow scrolling
        return false;
    }

    if ( ( mevent.state & GDK_BUTTON1_MASK ) && !this->grab && this->is_drawing) {
        /* Grab mouse, so release will not pass unnoticed */
        this->grab = SP_CANVAS_ITEM(desktop->acetate);
        sp_canvas_item_grab(this->grab, ( GDK_KEY_PRESS_MASK | GDK_BUTTON_PRESS_MASK   |
                                        GDK_BUTTON_RELEASE_MASK |
                                        GDK_POINTER_MOTION_MASK  ),
                            NULL, mevent.time);
    }

    /* Find desktop coordinates */
    Geom::Point p = desktop->w2d(Geom::Point(mevent.x, mevent.y));

    /* Test whether we hit any anchor. */
    SPDrawAnchor *anchor = spdc_test_inside(this, Geom::Point(mevent.x,mevent.y));
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (pencil_within_tolerance) {
        gint const tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);
        if ( Geom::LInfty( Geom::Point(mevent.x,mevent.y) - pencil_drag_origin_w ) < tolerance ) {
            return false;   // Do not drag if we're within tolerance from origin.
        }
    }

    // Once the user has moved farther than tolerance from the original location
    // (indicating they intend to move the object, not click), then always process the
    // motion notify coordinates as given (no snapping back to origin)
    if (input_has_pressure && pencil_within_tolerance) {
        p = desktop->w2d(pencil_drag_origin_w);
        anchor = spdc_test_inside(this, pencil_drag_origin_w);
    }
    pencil_within_tolerance = false;

    switch (this->state) {
        case SP_PENCIL_CONTEXT_ADDLINE:
            /* Set red endpoint */
            if (input_has_pressure) {
                this->state = SP_PENCIL_CONTEXT_FREEHAND;
                return false;
            } 
            if (anchor) {
                p = anchor->dp;
            } else {
                Geom::Point ptnr(p);
                this->_endpointSnap(ptnr, mevent.state);
                p = ptnr;
            }
            this->_setEndpoint(p);
            ret = true;
            break;
        default:
            /* We may be idle or already freehand */
            if ( (mevent.state & GDK_BUTTON1_MASK) && this->is_drawing ) {
                if (this->state == SP_PENCIL_CONTEXT_IDLE) {
                    sp_event_context_discard_delayed_snap_event(this);
                }
                this->state = SP_PENCIL_CONTEXT_FREEHAND;

                if ( !this->sa && !this->green_anchor ) {
                    /* Create green anchor */
                    this->green_anchor = sp_draw_anchor_new(this, this->green_curve, TRUE, this->p[0]);
                }
                if (anchor) {
                    std::cout << "aaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;
                    p = anchor->dp;
                }
                if ( this->npoints != 0) { // buttonpress may have happened before we entered draw context!
                    if (this->ps.empty()) {
                        // Only in freehand mode we have to add the first point also to this->ps (apparently)
                        // - We cannot add this point in spdc_set_startpoint, because we only need it for freehand
                        // - We cannot do this in the button press handler because at that point we don't know yet
                        //   wheter we're going into freehand mode or not
                        this->ps.push_back(this->p[0]);
                        this->wps.push_back(this->pressure);
                    }
                    this->_addFreehandPoint(p, mevent.state);
                    ret = true;
                }
                if (anchor && !this->anchor_statusbar) {
                    this->message_context->set(Inkscape::NORMAL_MESSAGE, _("<b>Release</b> here to close and finish the path."));
                    this->anchor_statusbar = true;
                } else if (!anchor && this->anchor_statusbar) {
                    this->message_context->clear();
                    this->anchor_statusbar = false;
                } else if (!anchor) {
                    this->message_context->set(Inkscape::NORMAL_MESSAGE, _("Drawing a freehand path"));
                }

            } else {
                if (anchor && !this->anchor_statusbar) {
                    this->message_context->set(Inkscape::NORMAL_MESSAGE, _("<b>Drag</b> to continue the path from this point."));
                    this->anchor_statusbar = true;
                } else if (!anchor && this->anchor_statusbar) {
                    this->message_context->clear();
                    this->anchor_statusbar = false;
                }
            }

            // Show the pre-snap indicator to communicate to the user where we would snap to if he/she were to
            // a) press the mousebutton to start a freehand drawing, or
            // b) release the mousebutton to finish a freehand drawing
            if (!this->sp_event_context_knot_mouseover()) {
                SnapManager &m = desktop->namedview->snap_manager;
                m.setup(desktop, true);
                m.preSnap(Inkscape::SnapCandidatePoint(p, Inkscape::SNAPSOURCE_NODE_HANDLE));
                m.unSetup();
            }
            break;
    }
    return ret;
}

bool PencilTool::_handleButtonRelease(GdkEventButton const &revent) {
    bool ret = false;

    if ( revent.button == 1 && this->is_drawing && !this->space_panning) {
        this->is_drawing = false;

        /* Find desktop coordinates */
        Geom::Point p = desktop->w2d(Geom::Point(revent.x, revent.y));

        /* Test whether we hit any anchor. */
        SPDrawAnchor *anchor = spdc_test_inside(this, Geom::Point(revent.x, revent.y));

        switch (this->state) {
            case SP_PENCIL_CONTEXT_IDLE:
                /* Releasing button in idle mode means single click */
                /* We have already set up start point/anchor in button_press */
                if (!(revent.state & GDK_CONTROL_MASK)) {
                    // Ctrl+click creates a single point so only set context in ADDLINE mode when Ctrl isn't pressed
                    this->state = SP_PENCIL_CONTEXT_ADDLINE;
                }
                break;
            case SP_PENCIL_CONTEXT_ADDLINE:
                /* Finish segment now */
                if (anchor) {
                    p = anchor->dp;
                } else {
                    this->_endpointSnap(p, revent.state);
                }
                this->ea = anchor;
                this->_setEndpoint(p);
                this->_finishEndpoint();
                this->state = SP_PENCIL_CONTEXT_IDLE;
                sp_event_context_discard_delayed_snap_event(this);
                break;
            case SP_PENCIL_CONTEXT_FREEHAND:
                if (revent.state & GDK_MOD1_MASK) {
                    /* sketch mode: interpolate the sketched path and improve the current output path with the new interpolation. don't finish sketch */
                    this->_sketchInterpolate();

                    if (this->green_anchor) {
                        this->green_anchor = sp_draw_anchor_destroy(this->green_anchor);
                    }

                    this->state = SP_PENCIL_CONTEXT_SKETCH;
                } else {
                    /* Finish segment now */
                    /// \todo fixme: Clean up what follows (Lauris)
                    if (anchor) {
                        p = anchor->dp;
                    } else {
                        Geom::Point p_end = p;
                        if (!input_has_pressure) {
                            this->_endpointSnap(p_end, revent.state);
                            if (p_end != p) {
                                // then we must have snapped!
                                this->_addFreehandPoint(p_end, revent.state);
                            }
                        }
                    }

                    this->ea = anchor;
                    /* Write curves to object */

                    desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Finishing freehand"));
                    this->_interpolate(true);
                    //Remove here because points are recalculated once finish. This is because we want live preview.
                    this->points.clear();
                    spdc_concat_colors_and_flush(this, FALSE);
                    this->sa = NULL;
                    this->ea = NULL;
                    this->ps.clear();
                    this->wps.clear();
                    if (this->green_anchor) {
                        this->green_anchor = sp_draw_anchor_destroy(this->green_anchor);
                    }
                    this->state = SP_PENCIL_CONTEXT_IDLE;
                    // reset sketch mode too
                    this->sketch_n = 0;
                }
                break;
            case SP_PENCIL_CONTEXT_SKETCH:
            default:
                break;
        }

        if (this->grab) {
            /* Release grab now */
            sp_canvas_item_ungrab(this->grab, revent.time);
            this->grab = NULL;
        }

        ret = true;
    }
    return ret;
}

void PencilTool::_cancel() {
    if (this->grab) {
        /* Release grab now */
        sp_canvas_item_ungrab(this->grab, 0);
        this->grab = NULL;
    }

    this->is_drawing = false;
    this->state = SP_PENCIL_CONTEXT_IDLE;
    sp_event_context_discard_delayed_snap_event(this);

    this->red_curve->reset();
    sp_canvas_bpath_set_bpath(SP_CANVAS_BPATH(this->red_bpath), NULL);
    for (auto i:this->green_bpaths) {
        sp_canvas_item_destroy(i);
    }
    this->green_bpaths.clear();
    this->green_curve->reset();
    if (this->green_anchor) {
        this->green_anchor = sp_draw_anchor_destroy(this->green_anchor);
    }

    this->message_context->clear();
    this->message_context->flash(Inkscape::NORMAL_MESSAGE, _("Drawing cancelled"));

    this->desktop->canvas->endForcedFullRedraws();
}

bool PencilTool::_handleKeyPress(GdkEventKey const &event) {
    bool ret = false;

    switch (get_latin_keyval(&event)) {
        case GDK_KEY_Up:
        case GDK_KEY_Down:
        case GDK_KEY_KP_Up:
        case GDK_KEY_KP_Down:
            // Prevent the zoom field from activation.
            if (!Inkscape::UI::held_only_control(event)) {
                ret = true;
            }
            break;
        case GDK_KEY_Escape:
            if (this->npoints != 0) {
                // if drawing, cancel, otherwise pass it up for deselecting
                if (this->state != SP_PENCIL_CONTEXT_IDLE) {
                    this->_cancel();
                    ret = true;
                }
            }
            break;
        case GDK_KEY_z:
        case GDK_KEY_Z:
            if (Inkscape::UI::held_only_control(event) && this->npoints != 0) {
                // if drawing, cancel, otherwise pass it up for undo
                if (this->state != SP_PENCIL_CONTEXT_IDLE) {
                    this->_cancel();
                    ret = true;
                }
            }
            break;
        case GDK_KEY_g:
        case GDK_KEY_G:
            if (Inkscape::UI::held_only_shift(event)) {
                this->desktop->selection->toGuides();
                ret = true;
            }
            break;
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
        case GDK_KEY_Meta_L:
        case GDK_KEY_Meta_R:
            if (this->state == SP_PENCIL_CONTEXT_IDLE) {
                this->desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("<b>Sketch mode</b>: holding <b>Alt</b> interpolates between sketched paths. Release <b>Alt</b> to finalize."));
            }
            break;
        default:
            break;
    }
    return ret;
}

bool PencilTool::_handleKeyRelease(GdkEventKey const &event) {
    bool ret = false;

    switch (get_latin_keyval(&event)) {
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
        case GDK_KEY_Meta_L:
        case GDK_KEY_Meta_R:
            if (this->state == SP_PENCIL_CONTEXT_SKETCH) {
                spdc_concat_colors_and_flush(this, false);
                this->sketch_n = 0;
                this->sa = NULL;
                this->ea = NULL;
                if (this->green_anchor) {
                    this->green_anchor = sp_draw_anchor_destroy(this->green_anchor);
                }
                this->state = SP_PENCIL_CONTEXT_IDLE;
                sp_event_context_discard_delayed_snap_event(this);
                this->desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Finishing freehand sketch"));
                ret = true;
            }
            break;
        default:
            break;
    }
    return ret;
}

/**
 * Reset points and set new starting point.
 */
void PencilTool::_setStartpoint(Geom::Point const &p) {
    this->npoints = 0;
    this->red_curve_is_valid = false;
    if (in_svg_plane(p)) {
        this->p[this->npoints++] = p;
    }
}

/**
 * Change moving endpoint position.
 * <ul>
 * <li>Ctrl constrains to moving to H/V direction, snapping in given direction.
 * <li>Otherwise we snap freely to whatever attractors are available.
 * </ul>
 *
 * Number of points is (re)set to 2 always, 2nd point is modified.
 * We change RED curve.
 */
void PencilTool::_setEndpoint(Geom::Point const &p) {
    if (this->npoints == 0) {
        return;
        /* May occur if first point wasn't in SVG plane (e.g. weird w2d transform, perhaps from bad
         * zoom setting).
         */
    }
    g_return_if_fail( this->npoints > 0 );

    this->red_curve->reset();
    if ( ( p == this->p[0] )
         || !in_svg_plane(p) )
    {
        this->npoints = 1;
    } else {
        this->p[1] = p;
        this->npoints = 2;

        this->red_curve->moveto(this->p[0]);
        this->red_curve->lineto(this->p[1]);
        this->red_curve_is_valid = true;
        if (!input_has_pressure) {
            sp_canvas_bpath_set_bpath(SP_CANVAS_BPATH(this->red_bpath), this->red_curve);
        }
    }
}

/**
 * Finalize addline.
 *
 * \todo
 * fixme: I'd like remove red reset from concat colors (lauris).
 * Still not sure, how it will make most sense.
 */
void PencilTool::_finishEndpoint() {
    if (this->red_curve->is_unset() || 
        this->red_curve->first_point() == this->red_curve->second_point())
    {
        this->red_curve->reset();
        sp_canvas_bpath_set_bpath(SP_CANVAS_BPATH(this->red_bpath), NULL);
    } else {
        /* Write curves to object. */
        spdc_concat_colors_and_flush(this, FALSE);
        this->sa = NULL;
        this->ea = NULL;
    }
}

void
PencilTool::_powerStrokePreview(Geom::Path const path, std::vector<Geom::Point> points, bool write)
{
    using namespace Inkscape::LivePathEffect;
    SPDocument * document = SP_ACTIVE_DOCUMENT;
    if (!document) {
        return;
    }
    Inkscape::XML::Document *xml_doc = document->getReprDoc();
    SPCurve * curve = new SPCurve();
    Geom::PathVector const pathv(path);
    if (!_powerpreview) {
        Inkscape::XML::Node *body = NULL;
        body = xml_doc->createElement("svg:path");
        body->setAttribute("sodipodi:insensitive", "true");
        sp_desktop_apply_style_tool(desktop, body, Glib::ustring("/tools/freehand/pencil").data(), false);
        _powerpreview = SP_SHAPE(SP_ITEM(SP_ACTIVE_DESKTOP->currentLayer())->appendChildRepr(body));
        _powerpreview->setCurve(curve, true);
        Inkscape::GC::release(body);
        SPCSSAttr *css = sp_css_attr_from_object(_powerpreview, SP_STYLE_FLAG_ALWAYS);
        const gchar * stroke = sp_repr_css_property(css, "stroke", "none");
        const gchar * fill = sp_repr_css_property(css, "fill", "none");
        if (!strcmp(fill, "none")) {
            fill   = "#000000";
        }
        if (!strcmp(stroke, "none")) {
            stroke = "#000000";
        }
        sp_repr_css_set_property (css, "fill", stroke);
        sp_repr_css_set_property (css, "stroke", fill);
        Glib::ustring css_str;
        sp_repr_css_write_string(css,css_str);
        body->setAttribute("style", css_str.c_str());
        gchar * pvector_str = sp_svg_write_path(pathv);
        _powerpreview->setAttribute("d" , pvector_str);
        g_free(pvector_str);
        SPLPEItem * lpeitem = dynamic_cast<SPLPEItem *>(_powerpreview);
        if (!lpeitem) {
            return;
        }
        Effect::createAndApply(POWERSTROKE, SP_ACTIVE_DESKTOP->doc(), SP_ITEM(_powerpreview));
        Effect* lpe = lpeitem->getCurrentLPE();
        lpe->getRepr()->setAttribute("sort_points", "true");
        lpe->getRepr()->setAttribute("interpolator_type", "CentripetalCatmullRom");
        lpe->getRepr()->setAttribute("interpolate_original", "true");
        lpe->getRepr()->setAttribute("interpolator_beta", "0.2");
        lpe->getRepr()->setAttribute("miter_limit", "100");
        lpe->getRepr()->setAttribute("linejoin_type", "miter");
        end_linecap_type = lpe->getRepr()->attribute("end_linecap_type");
    }
    SPLPEItem * lpeitem = dynamic_cast<SPLPEItem *>(_powerpreview);
    if (!lpeitem) {
        return;
    }
    Effect* lpe = lpeitem->getCurrentLPE();
    lpe->getRepr()->setAttribute("end_linecap_type", end_linecap_type);
    static_cast<LPEPowerStroke*>(lpe)->offset_points.param_set_and_write_new_value(points);
    gchar * pvector_str = sp_svg_write_path(pathv);
    _powerpreview->setAttribute("inkscape:original-d" , pvector_str);
    g_free(pvector_str);
    if (write) {
        lpe->getRepr()->setAttribute("end_linecap_type", "round");
        curve = _powerpreview->getCurve();
        _powerpreview->setAttribute("inkscape:original-d", "");
        _powerpreview->setAttribute("d", "");
        if (!_powerpreviewtail) {
            Inkscape::XML::Node *tail = NULL;
            tail = xml_doc->createElement("svg:path");
            tail->setAttribute("sodipodi:insensitive", "true");
            _powerpreviewtail = SP_SHAPE(SP_ITEM(SP_ACTIVE_DESKTOP->currentLayer())->appendChildRepr(tail));
            SPCSSAttr *css = sp_css_attr_from_object(_powerpreview, SP_STYLE_FLAG_ALWAYS);
            Glib::ustring css_str;
            sp_repr_css_write_string(css,css_str);
            tail->setAttribute("style", css_str.c_str());
            Inkscape::GC::release(tail);
        }
        if (!curve->is_empty()) {
            _powerpreviewtail->setCurve(curve, true);
            _preview_ok = true;
        } else {//if (!_second_chance_preview) {
            _preview_ok = false;
        }
    }
    curve->unref();
}

void
PencilTool::removePowerStrokePreview()
{
    if (_powerpreviewtail) {
        _powerpreviewtail->deleteObject();
        _powerpreviewtail = NULL;
    }
    if(_powerpreview) {
        using namespace Inkscape::LivePathEffect;
        Effect* lpe = SP_LPE_ITEM(_powerpreview)->getCurrentLPE();
        SP_LPE_ITEM(_powerpreview)->removeCurrentPathEffect(true);
        LivePathEffectObject * lpeobj = lpe->getLPEObj();
        if (lpeobj) {
            SP_OBJECT(lpeobj)->deleteObject();
            lpeobj = NULL;
        }
        _powerpreview->deleteObject();
        _powerpreview = NULL;
    }
}
void 
PencilTool::addPowerStrokePencil(SPCurve * c) 
{
    using namespace Inkscape::LivePathEffect;
    this->points.clear();
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double tol          = prefs->getDoubleLimited("/tools/freehand/pencil/tolerance", 10.0, 1.0, 100.0);
    double gap_pressure = prefs->getIntLimited("/tools/freehand/pencil/ps-step-pressure",10, 1, 100)/100.0;
    double min          = prefs->getIntLimited("/tools/freehand/pencil/minpressure", 0, 1, 100) / 100.0;
    double max          = prefs->getIntLimited("/tools/freehand/pencil/maxpressure", 100, 1, 100) / 100.0;
    if (min > max){
        min = max;
    }
    bool live = false;
    SPCurve * curve;
    if (sa) {
        Effect* lpe = SP_LPE_ITEM(white_item)->getCurrentLPE();
        LPEPowerStroke* ps = static_cast<LPEPowerStroke*>(lpe);
        if (ps) {
            if (sa->start) {
                this->points = ps->offset_points.reverse_controlpoints(false);
            } else {
                this->points = ps->offset_points.data();
            }
        }
    }
    if(!c) {
        live = true;
        //Simplify a bit the base curve to avoid artifacts
        SPCurve * previous_red = red_curve->copy();
        SPCurve * previous_green = green_curve->copy();
        prefs->setDouble("/tools/freehand/pencil/tolerance", 18.0);
        std::vector<Geom::Point> stroreps = this->ps;
        std::vector<double> strorewps = this->wps;
        _interpolate(true);
        this->ps = stroreps;
        this->wps = strorewps;
        stroreps.clear();
        strorewps.clear();
        prefs->setDouble("/tools/freehand/pencil/tolerance", tol);
        if (sa) {
            curve = sa->curve;
            if(prefs->getInt("/tools/freehand/pencil/freehand-mode", 0) == 1 || 
               prefs->getInt("/tools/freehand/pencil/freehand-mode", 0) == 2)
            {
                curve = overwrite_curve;
            }
            if (sa->start) {
                SPCurve *ret = curve->create_reverse();
                curve->unref();
                curve = ret->copy();
                ret->unref();
            }
            if (!green_curve->is_empty()) {
                if (curve->is_empty()) {
                    curve = green_curve->copy();
                } else {
                    green_curve->move_endpoints(curve->first_path()->finalPoint(), green_curve->first_path()->finalPoint());
                    curve->append_continuous( green_curve, 0.0625);
                }
                if (!red_curve->is_empty()) {
                    curve->append_continuous( red_curve, 0.0625);
                }
            }
        } else {
            if (!green_curve->is_empty()) {
                curve = green_curve->copy();
                if (!red_curve->is_empty()) {
                    curve->append_continuous( red_curve, 0.0625);
                }
            } else {
                curve = NULL;
            }
        }
        red_curve = previous_red->copy();
        green_curve = previous_green->copy();
        previous_red->unref();
        previous_green->unref();
    } else {
        curve = c->copy();
        removePowerStrokePreview();
    }
    SPDocument * document = SP_ACTIVE_DOCUMENT;
    if (!document) {
        return;
    }
    if (!curve) {
        return;
    }
    Geom::Affine transformCoordinate = SP_ITEM(SP_ACTIVE_DESKTOP->currentLayer())->i2dt_affine();
    Geom::PathVector pathvector = curve->get_pathvector();
    Geom::Path const path = pathvector[0];
    curve->unref();
    double previous_pressure = 0.0;
    double pressure_computed = 0.0;
    double dezoomify_factor = 0.05 * 1000/SP_EVENT_CONTEXT(this)->desktop->current_zoom();//\/100 we want 100% = 1;
    bool start     = true;
    auto pressure  = this->wps.begin();
    size_t counter = 0;
    Geom::Point position = *this->ps.begin();
    if (!live) {
        position *= transformCoordinate.inverse();
    }
    double pos = Geom::nearest_time(position, path);
    for (auto point = this->ps.begin(); point != this->ps.end(); ++point, ++pressure) {
        counter++;
        double pressure_shrunk = (*pressure * (max - min)) + min;
       //We need half width for power stroke
        pressure_computed = pressure_shrunk * dezoomify_factor/2.0;
        //remove start pressure gap
        if (start) {
            start = false;
            this->points.push_back(Geom::Point(pos + 0.01, pressure_computed));
            previous_pressure = pressure_shrunk;
            continue;
        }
        position = *point;
        if (!live) {
            position *= transformCoordinate.inverse();
        }
        pos = Geom::nearest_time(position, path);
        if (pos < 1e6 && std::abs(previous_pressure - pressure_shrunk) > gap_pressure && pos < path.size() - 1) {
            previous_pressure = pressure_shrunk;
            this->points.push_back(Geom::Point(pos, pressure_computed));
        }
    }
    if (live && this->points.size() > 0) {
        bool write = false;
        if (points_parsed != this->points.size() || !_preview_ok) {
            points_parsed = this->points.size();
            write = true;
        }
        if (write) {
            _powerStrokePreview(path * transformCoordinate.inverse(), this->points, write);
        } else {
            Geom::Point start = this->points[this->points.size()-1];
            if (start[Geom::X] < path.size()) {
                std::vector<Geom::Point> pointsend;
                Geom::Path const pathtail = path.portion(start[Geom::X], path.size());
                pointsend.push_back(Geom::Point(0.01, start[Geom::Y]));
                _powerStrokePreview(pathtail * transformCoordinate.inverse(), pointsend,  write);
                pointsend.clear();
            }
        }
    }
}

void PencilTool::_addFreehandPoint(Geom::Point const &p, guint /*state*/) {
    g_assert( this->npoints > 0 );
    g_return_if_fail(unsigned(this->npoints) < G_N_ELEMENTS(this->p));

    if ( ( p != this->p[ this->npoints - 1 ] )
         && in_svg_plane(p) )
    {
        this->p[this->npoints++] = p;
        this->_fitAndSplit();
        this->ps.push_back(p);
        this->wps.push_back(this->pressure);
        if (input_has_pressure) {
            this->addPowerStrokePencil(NULL);
        }
    }
}

static inline double
square(double const x)
{
    return x * x;
}

void PencilTool::_interpolate(bool realize) {
    if ( this->ps.size() <= 1 ) {
        return;
    }
    
    using Geom::X;
    using Geom::Y;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    //Remove powerstroke arifacts with input_has_pressure min clamp
    double tol = prefs->getDoubleLimited("/tools/freehand/pencil/tolerance", 10.0, 1.0, 100.0) * 0.4;
    bool simplify = prefs->getInt("/tools/freehand/pencil/simplify", 0);
    if(simplify){
        double tol2 = prefs->getDoubleLimited("/tools/freehand/pencil/base-simplify", 25.0, 1.0, 100.0) * 0.4;
        tol = std::min(tol,tol2);
    }
    //To avoid artifacts on pressure power
    if (input_has_pressure) {
        if (tol < 18.0 * 0.4) {
            tol = 18.0 * 0.4;
        }
        //we dont need a exact calulation set up a high precission
        //we remove pointa at start and end nearest to 1/20 of total length
        double distance = 0;
        Geom::Point prev = this->ps[0];
        for (auto i:this->ps) {
            if (i == prev) {
                continue;
            }
            distance += Geom::distance(i, prev);
            prev = i;
        }
        double smoothlenght = (distance/this->ps.size()); 
        std::cout << smoothlenght << "smoothlenght" << std::endl;

//        //Double check to limit on large strokes
//        double limitlenght = desktop->get_display_area().diameter()/20.0;
//        std::cout << limitlenght << "limitlenght" << std::endl;
//        //smoothlenght = std::min(smoothlenght, limitlenght);
//        //Smooth start segments




        if (realize && this->ps.size() > 3) {
            Geom::Point start_point = *this->ps.begin();
            while ( this->ps.size() > 6 && Geom::distance(*(this->ps.begin()+1), start_point) < smoothlenght) {
                this->ps.erase(this->ps.begin() + 1);
                this->wps.erase(this->wps.begin() + 1);
            }
        }
        //Smooth last segments
        if (realize && this->ps.size() > 3) {
            Geom::Point last_point = *this->ps.end();
            bool erased = false;
            while ( this->ps.size() > 6 && Geom::distance(*this->ps.end(), last_point) < smoothlenght) {
                this->ps.pop_back();
                this->wps.pop_back();
                erased = true;
            }
            if (erased) {
                this->wps.push_back(this->wps[this->wps.size()-1]);
                this->ps.push_back(last_point);
            }
        }
    }

    double tolerance_sq = 0.02 * square(this->desktop->w2d().descrim() * tol) * exp(0.2 * tol - 2);

    g_assert(is_zero(this->req_tangent) || is_unit_vector(this->req_tangent));

    this->green_curve->reset();
    this->red_curve->reset();
    this->red_curve_is_valid = false;

    int n_points = this->ps.size();

    // worst case gives us a segment per point
    int max_segs = 4 * n_points;

    std::vector<Geom::Point> b(max_segs);

    int const n_segs = Geom::bezier_fit_cubic_r(b.data(), this->ps.data(), n_points, tolerance_sq, max_segs);

    if (n_segs > 0) {
        /* Fit and draw and reset state */

        this->green_curve->moveto(b[0]);
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        guint mode = prefs->getInt("/tools/freehand/pencil/freehand-mode", 0);
        for (int c = 0; c < n_segs; c++) {
            // if we are in BSpline we modify the trace to create adhoc nodes 
            if (mode == 2) {
                Geom::Point point_at1 = b[4 * c + 0] + (1./3) * (b[4 * c + 3] - b[4 * c + 0]);
                point_at1 = Geom::Point(point_at1[X] + HANDLE_CUBIC_GAP, point_at1[Y] + HANDLE_CUBIC_GAP);
                Geom::Point point_at2 = b[4 * c + 3] + (1./3) * (b[4 * c + 0] - b[4 * c + 3]);
                point_at2 = Geom::Point(point_at2[X] + HANDLE_CUBIC_GAP, point_at2[Y] + HANDLE_CUBIC_GAP);
                this->green_curve->curveto(point_at1,point_at2,b[4*c+3]);
            } else {
                //force retracted handle at end if power stroke
                if (c == n_segs - 1 && input_has_pressure) {
                    this->green_curve->curveto(b[4 * c + 1], b[4 * c + 3], b[4 * c + 3]);
                } else if (c == 0 && input_has_pressure) {
                    this->green_curve->curveto(b[4 * c], b[4 * c + 2], b[4 * c + 3]);
                } else {
                    this->green_curve->curveto(b[4 * c + 1], b[4 * c + 2], b[4 * c + 3]);
                }
            }
        }
        if (!input_has_pressure) {
            sp_canvas_bpath_set_bpath(SP_CANVAS_BPATH(this->red_bpath), this->green_curve);
        }
        
        /* Fit and draw and copy last point */
        g_assert(!this->green_curve->is_empty());

        /* Set up direction of next curve. */
        {
            Geom::Curve const * last_seg = this->green_curve->last_segment();
            g_assert( last_seg );      // Relevance: validity of (*last_seg)
            this->p[0] = last_seg->finalPoint();
            this->npoints = 1;
            Geom::Curve *last_seg_reverse = last_seg->reverse();
            Geom::Point const req_vec( -last_seg_reverse->unitTangentAt(0) );
            delete last_seg_reverse;
            this->req_tangent = ( ( Geom::is_zero(req_vec) || !in_svg_plane(req_vec) )
                                ? Geom::Point(0, 0)
                                : Geom::unit_vector(req_vec) );
        }
    }
}


/* interpolates the sketched curve and tweaks the current sketch interpolation*/
void PencilTool::_sketchInterpolate() {
    if ( this->ps.size() <= 1 ) {
        return;
    }

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double tol = prefs->getDoubleLimited("/tools/freehand/pencil/tolerance", 10.0, 1.0, 100.0) * 0.4;
    bool simplify = prefs->getInt("/tools/freehand/pencil/simplify", 0);
    if(simplify){
        double tol2 = prefs->getDoubleLimited("/tools/freehand/pencil/base-simplify", 25.0, 1.0, 100.0) * 0.4;
        tol = std::min(tol,tol2);
    }
    double tolerance_sq = 0.02 * square(this->desktop->w2d().descrim() * tol) * exp(0.2 * tol - 2);
    
    bool average_all_sketches = prefs->getBool("/tools/freehand/pencil/average_all_sketches", true);

    g_assert(is_zero(this->req_tangent) || is_unit_vector(this->req_tangent));

    this->red_curve->reset();
    this->red_curve_is_valid = false;

    int n_points = this->ps.size();

    // worst case gives us a segment per point
    int max_segs = 4 * n_points;

    std::vector<Geom::Point> b(max_segs);

    int const n_segs = Geom::bezier_fit_cubic_r(b.data(), this->ps.data(), n_points, tolerance_sq, max_segs);

    if (n_segs > 0) {
        Geom::Path fit(b[0]);

        for (int c = 0; c < n_segs; c++) {
            fit.appendNew<Geom::CubicBezier>(b[4 * c + 1], b[4 * c + 2], b[4 * c + 3]);
        }

        Geom::Piecewise<Geom::D2<Geom::SBasis> > fit_pwd2 = fit.toPwSb();

        if (this->sketch_n > 0) {
            double t;

            if (average_all_sketches) {
                // Average = (sum of all) / n
                //         = (sum of all + new one) / n+1
                //         = ((old average)*n + new one) / n+1
                t = this->sketch_n / (this->sketch_n + 1.);
            } else {
                t = 0.5;
            }

            this->sketch_interpolation = Geom::lerp(t, fit_pwd2, this->sketch_interpolation);

            // simplify path, to eliminate small segments
            Path path;
            path.LoadPathVector(Geom::path_from_piecewise(this->sketch_interpolation, 0.01));
            path.Simplify(0.5);

            Geom::PathVector *pathv = path.MakePathVector();
            this->sketch_interpolation = (*pathv)[0].toPwSb();
            delete pathv;
        } else {
            this->sketch_interpolation = fit_pwd2;
        }

        this->sketch_n++;

        this->green_curve->reset();
        this->green_curve->set_pathvector(Geom::path_from_piecewise(this->sketch_interpolation, 0.01));
        if (!input_has_pressure) {
            sp_canvas_bpath_set_bpath(SP_CANVAS_BPATH(this->red_bpath), this->green_curve);
        }
        /* Fit and draw and copy last point */
        g_assert(!this->green_curve->is_empty());

        /* Set up direction of next curve. */
        {
            Geom::Curve const * last_seg = this->green_curve->last_segment();
            g_assert( last_seg );      // Relevance: validity of (*last_seg)
            this->p[0] = last_seg->finalPoint();
            this->npoints = 1;
            Geom::Curve *last_seg_reverse = last_seg->reverse();
            Geom::Point const req_vec( -last_seg_reverse->unitTangentAt(0) );
            delete last_seg_reverse;
            this->req_tangent = ( ( Geom::is_zero(req_vec) || !in_svg_plane(req_vec) )
                                ? Geom::Point(0, 0)
                                : Geom::unit_vector(req_vec) );
        }
    }

    this->ps.clear();
    this->points.clear();
    this->wps.clear();
}

void PencilTool::_fitAndSplit() {
    g_assert( this->npoints > 1 );

    double const tolerance_sq = 0;

    Geom::Point b[4];
    g_assert(is_zero(this->req_tangent)
             || is_unit_vector(this->req_tangent));
    Geom::Point const tHatEnd(0, 0);
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    int const n_segs = Geom::bezier_fit_cubic_full(b, NULL, this->p, this->npoints,
                                                this->req_tangent, tHatEnd,
                                                tolerance_sq, 1);
    if ( n_segs > 0
         && unsigned(this->npoints) < G_N_ELEMENTS(this->p) )
    {
        /* Fit and draw and reset state */

        this->red_curve->reset();
        this->red_curve->moveto(b[0]);
        using Geom::X;
        using Geom::Y;
            // if we are in BSpline we modify the trace to create adhoc nodes
        guint mode = prefs->getInt("/tools/freehand/pencil/freehand-mode", 0);
        if(mode == 2){
            Geom::Point point_at1 = b[0] + (1./3)*(b[3] - b[0]);
            point_at1 = Geom::Point(point_at1[X] + HANDLE_CUBIC_GAP, point_at1[Y] + HANDLE_CUBIC_GAP);
            Geom::Point point_at2 = b[3] + (1./3)*(b[0] - b[3]);
            point_at2 = Geom::Point(point_at2[X] + HANDLE_CUBIC_GAP, point_at2[Y] + HANDLE_CUBIC_GAP);
            this->red_curve->curveto(point_at1,point_at2,b[3]);
        }else{
            this->red_curve->curveto(b[1], b[2], b[3]);
        }
        if (!input_has_pressure) {
            sp_canvas_bpath_set_bpath(SP_CANVAS_BPATH(this->red_bpath), this->red_curve);
        }
        this->red_curve_is_valid = true;
    } else {
        /* Fit and draw and copy last point */

        g_assert(!this->red_curve->is_empty());

        /* Set up direction of next curve. */
        {
            Geom::Curve const * last_seg = this->red_curve->last_segment();
            g_assert( last_seg );      // Relevance: validity of (*last_seg)
            this->p[0] = last_seg->finalPoint();
            this->npoints = 1;
            Geom::Curve *last_seg_reverse = last_seg->reverse();
            Geom::Point const req_vec( -last_seg_reverse->unitTangentAt(0) );
            delete last_seg_reverse;
            this->req_tangent = ( ( Geom::is_zero(req_vec) || !in_svg_plane(req_vec) )
                                ? Geom::Point(0, 0)
                                : Geom::unit_vector(req_vec) );
        }


        this->green_curve->append_continuous(this->red_curve, 0.0625);
        SPCurve *curve = this->red_curve->copy();

        /// \todo fixme:
        SPCanvasItem *cshape = sp_canvas_bpath_new(this->desktop->getSketch(), curve, true);
        curve->unref();

        this->highlight_color = SP_ITEM(this->desktop->currentLayer())->highlight_color();
        if((unsigned int)prefs->getInt("/tools/nodes/highlight_color", 0xff0000ff) == this->highlight_color){
            this->green_color = 0x00ff007f;
        } else {
            this->green_color = this->highlight_color;
        }
        sp_canvas_bpath_set_stroke(SP_CANVAS_BPATH(cshape), this->green_color, 1.0, SP_STROKE_LINEJOIN_MITER, SP_STROKE_LINECAP_BUTT);

        this->green_bpaths.push_back(cshape);

        this->red_curve_is_valid = false;
    }
}

}
}
}

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
