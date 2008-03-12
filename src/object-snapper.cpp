/**
 *  \file object-snapper.cpp
 *  \brief Snapping things to objects.
 *
 * Authors:
 *   Carl Hetherington <inkscape@carlh.net>
 *   Diederik van Lierop <mail@diedenrezi.nl>
 *
 * Copyright (C) 2005 - 2008 Authors
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include "libnr/n-art-bpath.h"
#include "libnr/nr-path.h"
#include "libnr/nr-rect-ops.h"
#include "libnr/nr-point-fns.h"
#include "live_effects/n-art-bpath-2geom.h"
#include "2geom/path-intersection.h"
#include "document.h"
#include "sp-namedview.h"
#include "sp-image.h"
#include "sp-item-group.h"
#include "sp-item.h"
#include "sp-use.h"
#include "display/curve.h"
#include "desktop.h"
#include "inkscape.h"
#include "prefs-utils.h"
#include "sp-text.h"
#include "sp-flowtext.h"
#include "text-editing.h"

Inkscape::ObjectSnapper::ObjectSnapper(SPNamedView const *nv, NR::Coord const d)
    : Snapper(nv, d), _snap_to_itemnode(true), _snap_to_itempath(true),
      _snap_to_bboxnode(true), _snap_to_bboxpath(true), _snap_to_page_border(false),
      _strict_snapping(true), _include_item_center(false)
{
    _candidates = new std::vector<SPItem*>;
    _points_to_snap_to = new std::vector<NR::Point>;
    _bpaths_to_snap_to = new std::vector<NArtBpath*>;
    _paths_to_snap_to = new std::vector<Path*>;
}

Inkscape::ObjectSnapper::~ObjectSnapper()
{
    _candidates->clear(); //Don't delete the candidates themselves, as these are not ours!
    delete _candidates;

    _points_to_snap_to->clear();
    delete _points_to_snap_to;

    _clear_paths();
    delete _paths_to_snap_to;
    delete _bpaths_to_snap_to;
}

/**
 *  Find all items within snapping range.
 *  \param r Pointer to the current document
 *  \param it List of items to ignore
 *  \param first_point If true then this point is the first one from a whole bunch of points
 *  \param points_to_snap The whole bunch of points, all from the same selection and having the same transformation
 *  \param DimensionToSnap Snap in X, Y, or both directions.
 */

void Inkscape::ObjectSnapper::_findCandidates(SPObject* r,
                                              std::list<SPItem const *> const &it,
                                              bool const &first_point,
                                              std::vector<NR::Point> &points_to_snap,
                                              DimensionToSnap const snap_dim) const
{
    bool const c1 = (snap_dim == TRANSL_SNAP_XY) && ThisSnapperMightSnap();
    bool const c2 = (snap_dim != TRANSL_SNAP_XY) && GuidesMightSnap();
    bool const c3 = points_to_snap.size() == 0;
    
    if (!(c1 || c2) || c3) {
        return;        
    }
    
    SPDesktop const *desktop = SP_ACTIVE_DESKTOP;

    if (first_point) {
        _candidates->clear();
    }
    
    NR::Maybe<NR::Rect> bbox = NR::Rect(); // a default NR::Rect is infinitely large
    NR::Coord t = getSnapperTolerance();
    
    // When dragging a guide...
    NR::Point p_guide = points_to_snap[0];
    if (!getSnapperAlwaysSnap()) {
        bbox = NR::Rect(p_guide, p_guide); // bbox is now just a single point: p_guide
        bbox->growBy(t); // bbox width and height now measure 2x snapper tolerance
        // for angled guidelines the bbox is now larger than really needed
        // (up to sqrt(2) for 45 deg. guidelines) but we'll leave it like that
    } // else: use an infinitely large bbox to find candidates 

    for (SPObject* o = sp_object_first_child(r); o != NULL; o = SP_OBJECT_NEXT(o)) {
        if (SP_IS_ITEM(o) && !SP_ITEM(o)->isLocked() && !desktop->itemIsHidden(SP_ITEM(o))) {

            /* See if this item is on the ignore list */
            std::list<SPItem const *>::const_iterator i = it.begin();
            while (i != it.end() && *i != o) {
                i++;
            }

            if (i == it.end()) {
                /* See if the item is within range */
                if (SP_IS_GROUP(o)) {
                    _findCandidates(o, it, false, points_to_snap, snap_dim);
                } else {
                    // Now let's see if any of the snapping points is within snapping range of this object
                    if (snap_dim == TRANSL_SNAP_XY) {
                        bbox = sp_item_bbox_desktop(SP_ITEM(o));
                    } // else: we're snapping a guide to an object and we will use the bbox as defined above
                    
                    if (bbox) {
                        for (std::vector<NR::Point>::const_iterator i = points_to_snap.begin(); i != points_to_snap.end(); i++) {
                            NR::Point b_min = bbox->min();
                            NR::Point b_max = bbox->max();
                            bool withinX = ((*i)[NR::X] >= b_min[NR::X] - t) && ((*i)[NR::X] <= b_max[NR::X] + t);
                            bool withinY = ((*i)[NR::Y] >= b_min[NR::Y] - t) && ((*i)[NR::Y] <= b_max[NR::Y] + t);
                            if (withinX && withinY) {
                                //We've found a point that is within snapping range
                                //of this object, so record it as a candidate
                                _candidates->push_back(SP_ITEM(o));
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}


void Inkscape::ObjectSnapper::_collectNodes(Inkscape::Snapper::PointType const &t,
                                         bool const &first_point) const
{
    // Now, let's first collect all points to snap to. If we have a whole bunch of points to snap,
    // e.g. when translating an item using the selector tool, then we will only do this for the
    // first point and store the collection for later use. This significantly improves the performance
    if (first_point) {
        _points_to_snap_to->clear();
        
         // Determine the type of bounding box we should snap to
        SPItem::BBoxType bbox_type = SPItem::GEOMETRIC_BBOX;
        
        bool p_is_a_node = t & Inkscape::Snapper::SNAPPOINT_NODE;
        bool p_is_a_bbox = t & Inkscape::Snapper::SNAPPOINT_BBOX;
        bool p_is_a_guide = t & Inkscape::Snapper::SNAPPOINT_GUIDE;
        
        // A point considered for snapping should be either a node, a bbox corner or a guide. Pick only ONE!
        g_assert(!(p_is_a_node && p_is_a_bbox || p_is_a_bbox && p_is_a_guide || p_is_a_node && p_is_a_guide));        
        
        if (_snap_to_bboxnode) {
            gchar const *prefs_bbox = prefs_get_string_attribute("tools", "bounding_box");
            bbox_type = (prefs_bbox != NULL && strcmp(prefs_bbox, "geometric")==0)? SPItem::GEOMETRIC_BBOX : SPItem::APPROXIMATE_BBOX;
        }

        for (std::vector<SPItem*>::const_iterator i = _candidates->begin(); i != _candidates->end(); i++) {
            //NR::Matrix i2doc(NR::identity());
            SPItem *root_item = *i;
            if (SP_IS_USE(*i)) {
                root_item = sp_use_root(SP_USE(*i));
            }
            g_return_if_fail(root_item);

            //Collect all nodes so we can snap to them
            if (_snap_to_itemnode) {
                if (!(_strict_snapping && !p_is_a_node) || p_is_a_guide) {
                    sp_item_snappoints(root_item, _include_item_center, SnapPointsIter(*_points_to_snap_to));
                }
            }

            //Collect the bounding box's corners so we can snap to them
            if (_snap_to_bboxnode) {
                if (!(_strict_snapping && !p_is_a_bbox) || p_is_a_guide) {
                    NR::Maybe<NR::Rect> b = sp_item_bbox_desktop(root_item, bbox_type);
                    if (b) {
                        for ( unsigned k = 0 ; k < 4 ; k++ ) {
                            _points_to_snap_to->push_back(b->corner(k));
                        }
                    }
                }
            }
        }
    }
}

void Inkscape::ObjectSnapper::_snapNodes(SnappedConstraints &sc,
                                         Inkscape::Snapper::PointType const &t,
                                         NR::Point const &p,
                                         bool const &first_point,
                                         std::vector<NR::Point> *unselected_nodes) const
{
    // Iterate through all nodes, find out which one is the closest to p, and snap to it!
    
    _collectNodes(t, first_point);
    
    if (unselected_nodes != NULL) {
        _points_to_snap_to->insert(_points_to_snap_to->end(), unselected_nodes->begin(), unselected_nodes->end());
    }   
    
    SnappedPoint s;
    bool success = false;
    
    for (std::vector<NR::Point>::const_iterator k = _points_to_snap_to->begin(); k != _points_to_snap_to->end(); k++) {
        NR::Coord dist = NR::L2(*k - p);        
        if (dist < getSnapperTolerance() && dist < s.getDistance()) {
            s = SnappedPoint(*k, dist, getSnapperTolerance(), getSnapperAlwaysSnap());
            success = true;
        }
    }

    if (success) {
    	sc.points.push_back(s);	
    }
}

void Inkscape::ObjectSnapper::_snapTranslatingGuideToNodes(SnappedConstraints &sc,
                                         Inkscape::Snapper::PointType const &t,
                                         NR::Point const &p,
                                         NR::Point const &guide_normal) const
{
    // Iterate through all nodes, find out which one is the closest to this guide, and snap to it!
    _collectNodes(t, true);
    
    SnappedPoint s;
    bool success = false;
    
    NR::Coord tol = getSnapperTolerance();
    
    for (std::vector<NR::Point>::const_iterator k = _points_to_snap_to->begin(); k != _points_to_snap_to->end(); k++) {
        // Project each node (*k) on the guide line (running through point p)
        NR::Point p_proj = project_on_linesegment(*k, p, p + NR::rot90(guide_normal));
        NR::Coord dist = NR::L2(*k - p_proj); // distance from node to the guide         
        NR::Coord dist2 = NR::L2(p - p_proj); // distance from projection of node on the guide, to the mouse location
        if ((dist < tol && dist2 < tol || getSnapperAlwaysSnap()) && dist < s.getDistance()) {
            s = SnappedPoint(*k, dist, tol, getSnapperAlwaysSnap());
            success = true;
        }
    }

    if (success) {
        sc.points.push_back(s); 
    }
}

void Inkscape::ObjectSnapper::_collectPaths(Inkscape::Snapper::PointType const &t,
                                         bool const &first_point,
                                         SPPath const *selected_path,
                                         NArtBpath *border_bpath) const
{
    // Now, let's first collect all paths to snap to. If we have a whole bunch of points to snap,
    // e.g. when translating an item using the selector tool, then we will only do this for the
    // first point and store the collection for later use. This significantly improves the performance
    if (first_point) {
        _clear_paths();
        
        // Determine the type of bounding box we should snap to
        SPItem::BBoxType bbox_type = SPItem::GEOMETRIC_BBOX;
        
        bool p_is_a_node = t & Inkscape::Snapper::SNAPPOINT_NODE;
        
        if (_snap_to_bboxpath) {
            gchar const *prefs_bbox = prefs_get_string_attribute("tools", "bounding_box");
            bbox_type = (prefs_bbox != NULL && strcmp(prefs_bbox, "geometric")==0)? SPItem::GEOMETRIC_BBOX : SPItem::APPROXIMATE_BBOX;
        }
        
        // Consider the page border for snapping
        if (border_bpath != NULL) { 
            _bpaths_to_snap_to->push_back(border_bpath);    
        }
        
        /* While editing a path in the node tool, findCandidates must ignore that path because 
         * of the node snapping requirements (i.e. only unselected nodes must be snapable).
         * This path must not be ignored however when snapping to the paths, so we add it here
         * manually when applicable. 
         * 
         * It must be the last one in the list, as this is assumption is being used in _snapPaths() 
         */ 
        if (_snap_to_itempath && selected_path != NULL) {
            _candidates->push_back(SP_ITEM(selected_path));            
        }
            
        for (std::vector<SPItem*>::const_iterator i = _candidates->begin(); i != _candidates->end(); i++) {

            /* Transform the requested snap point to this item's coordinates */
            NR::Matrix i2doc(NR::identity());
            SPItem *root_item = NULL;
            /* We might have a clone at hand, so make sure we get the root item */
            if (SP_IS_USE(*i)) {
                i2doc = sp_use_get_root_transform(SP_USE(*i));
                root_item = sp_use_root(SP_USE(*i));
                g_return_if_fail(root_item);
            } else {
                i2doc = sp_item_i2doc_affine(*i);
                root_item = *i;
            }

            //Build a list of all paths considered for snapping to

            //Add the item's path to snap to
            if (_snap_to_itempath) {
                if (!(_strict_snapping && !p_is_a_node)) {
                    // Snapping to the path of characters is very cool, but for a large
                    // chunk of text this will take ages! So limit snapping to text paths
                    // containing max. 240 characters. Snapping the bbox will not be affected
                    bool very_lenghty_prose = false;
                    if (SP_IS_TEXT(root_item) || SP_IS_FLOWTEXT(root_item)) {
                        very_lenghty_prose =  sp_text_get_length(SP_TEXT(root_item)) > 240;
                    }
                    // On my AMD 3000+, the snapping lag becomes annoying at approx. 240 chars
                    // which corresponds to a lag of 500 msec. This is for snapping a rect
                    // to a single line of text.

                    // Snapping for example to a traced bitmap is also very stressing for
                    // the CPU, so we'll only snap to paths having no more than 500 nodes
                    // This also leads to a lag of approx. 500 msec (in my lousy test set-up).
                    bool very_complex_path = false;
                    if (SP_IS_PATH(root_item)) {
                        very_complex_path = sp_nodes_in_path(SP_PATH(root_item)) > 500;
                    }

                    if (!very_lenghty_prose && !very_complex_path) {
                        SPCurve *curve = curve_for_item(root_item); 
                        if (curve) {
                            NArtBpath *bpath = bpath_for_curve(root_item, curve, true, true);
                            _bpaths_to_snap_to->push_back(bpath);
                            // Because we set doTransformation to true in bpath_for_curve, we
                            // will get a dupe of the path, which must be freed at some point
                            sp_curve_unref(curve);
                        }
                    }
                }
            }

            //Add the item's bounding box to snap to
            if (_snap_to_bboxpath) {
                if (!(_strict_snapping && p_is_a_node)) {
                    NRRect rect;
                    sp_item_invoke_bbox(root_item, &rect, i2doc, TRUE, bbox_type);
                    NArtBpath *bpath = nr_path_from_rect(rect);
                    _bpaths_to_snap_to->push_back(bpath);
                }
            }
        }
    }
}
    
void Inkscape::ObjectSnapper::_snapPaths(SnappedConstraints &sc,
                                     Inkscape::Snapper::PointType const &t,
                                     NR::Point const &p,
                                     bool const &first_point,
                                     std::vector<NR::Point> *unselected_nodes,
                                     SPPath const *selected_path,
                                     NArtBpath *border_bpath) const
{
    _collectPaths(t, first_point, selected_path, border_bpath);
    
    // Now we can finally do the real snapping, using the paths collected above
    SnappedPoint s;
    bool success = false;
    
    /* FIXME: this seems like a hack.  Perhaps Snappers should be
    ** in SPDesktop rather than SPNamedView?
    */
    SPDesktop const *desktop = SP_ACTIVE_DESKTOP;    
    NR::Point const p_doc = desktop->dt2doc(p);    
    
    // Convert all bpaths to Paths, because here we really must have Paths
    // (whereas in _snapPathsConstrained we will use the original bpaths)
    if (first_point) {
        for (std::vector<NArtBpath*>::const_iterator k = _bpaths_to_snap_to->begin(); k != _bpaths_to_snap_to->end(); k++) {
            Path *path = bpath_to_Path(*k);
            if (path) {
                path->ConvertWithBackData(0.01); //This is extremely time consuming!
                _paths_to_snap_to->push_back(path);
            }    
        }
    }
    
    for (std::vector<Path*>::const_iterator k = _paths_to_snap_to->begin(); k != _paths_to_snap_to->end(); k++) {
        if (*k) {
            bool being_edited = false; // True if the current path is being edited in the node tool            
            if (unselected_nodes != NULL) {
                if (unselected_nodes->size() > 0 && selected_path != NULL) { // If the node tool is active ...
                    if (*k == _paths_to_snap_to->back()) { // and we arrived at the last path in the vector ...
                        being_edited = true;  // then this path is currently being edited
                    }        
                }
            }
            
            for (unsigned i = 1 ; i < (*k)->pts.size() ; i++) {
                NR::Point start_point;
                NR::Point end_point;   
                NR::Maybe<Path::cut_position> o = NR::Nothing();                 
                if (being_edited) { 
                    /* If the path is being edited, then we will try to snap to each piece of the 
                     * path individually. We should only snap though to stationary pieces of the paths
                     * and not to the pieces that are being dragged around. This way we avoid 
                     * self-snapping. For this we check whether the nodes at both ends of the current
                     * piece are unselected; if they are then this piece must be stationary 
                     */
                    (*k)->PointAt(i, 0, start_point);
                    (*k)->PointAt(i, 1, end_point);
                    start_point = desktop->doc2dt(start_point);
                    end_point = desktop->doc2dt(end_point);
                    g_assert(unselected_nodes != NULL);
                    bool c1 = isUnselectedNode(start_point, unselected_nodes);
                    bool c2 = isUnselectedNode(end_point, unselected_nodes);     
                    if (c1 && c2) {
                        o = get_nearest_position_on_Path(*k, p_doc, i);
                    }
                } else {
                    /* If the path is NOT being edited, then we will try to snap to the path as a
                     * whole, so we need to do this only once and we will break out at the end of
                     * this for-loop iteration */                    
                    /* Look for the nearest position on this SPItem to our snap point */
                    o = get_nearest_position_on_Path(*k, p_doc);
                    (*k)->PointAt(o->piece, 0, start_point);
                    (*k)->PointAt(o->piece, 1, end_point);
                    start_point = desktop->doc2dt(start_point);
                    end_point = desktop->doc2dt(end_point);        
                }
                
                if (o && o->t >= 0 && o->t <= 1) {    
                    /* Convert the nearest point back to desktop coordinates */
                    NR::Point const o_it = get_point_on_Path(*k, o->piece, o->t);
                    NR::Point const o_dt = desktop->doc2dt(o_it);                
                    NR::Coord const dist = NR::L2(o_dt - p);
    
                    if (dist < getSnapperTolerance()) {
                        // if we snap to a straight line segment (within a path), then return this line segment
                        if ((*k)->IsLineSegment(o->piece)) {
                            sc.lines.push_back(Inkscape::SnappedLineSegment(o_dt, dist, getSnapperTolerance(), getSnapperAlwaysSnap(), start_point, end_point));    
                        } else {                
                            // for segments other than straight lines of a path, we'll return just the closest snapped point
                            if (dist < s.getDistance()) {
                                s = SnappedPoint(o_dt, dist, getSnapperTolerance(), getSnapperAlwaysSnap());
                                success = true;
                            }
                        }
                    }
                }        
                
                // If the path is NOT being edited, then we will try to snap to the path as a whole
                // so we need to do this only once
                if (!being_edited) break;
            }
        }
    }

    if (success) {
    	sc.points.push_back(s);	
    }
}

/* Returns true if point is coincident with one of the unselected nodes */ 
bool Inkscape::ObjectSnapper::isUnselectedNode(NR::Point const &point, std::vector<NR::Point> const *unselected_nodes) const
{
    if (unselected_nodes == NULL) {
        return false;
    }
    
    if (unselected_nodes->size() == 0) {
        return false;
    }
    
    for (std::vector<NR::Point>::const_iterator i = unselected_nodes->begin(); i != unselected_nodes->end(); i++) {
        if (NR::L2(point - *i) < 1e-4) {
            return true;
        }   
    }  
    
    return false;    
}

void Inkscape::ObjectSnapper::_snapPathsConstrained(SnappedConstraints &sc,
                                     Inkscape::Snapper::PointType const &t,
                                     NR::Point const &p,
                                     bool const &first_point,
                                     ConstraintLine const &c) const
{
    
    // Consider the page's border for snapping to
    NArtBpath *border_bpath = _snap_to_page_border ? _getBorderBPath() : NULL;
    
    _collectPaths(t, first_point, NULL, border_bpath);
    
    // Now we can finally do the real snapping, using the paths collected above
    
    /* FIXME: this seems like a hack.  Perhaps Snappers should be
    ** in SPDesktop rather than SPNamedView?
    */
    SPDesktop const *desktop = SP_ACTIVE_DESKTOP;    
    NR::Point const p_doc = desktop->dt2doc(p);    
    
    NR::Point direction_vector = c.getDirection();
    if (!is_zero(direction_vector)) {
        direction_vector = NR::unit_vector(direction_vector);
    }
    
    NR::Point const p1_on_cl = c.hasPoint() ? c.getPoint() : p;
    NR::Point const p2_on_cl = p1_on_cl + direction_vector;
    
    // The intersection point of the constraint line with any path, 
    // must lie within two points on the constraintline: p_min_on_cl and p_max_on_cl
    // The distance between those points is twice the snapping tolerance
    NR::Point const p_proj_on_cl = project_on_linesegment(p, p1_on_cl, p2_on_cl);
    NR::Point const p_min_on_cl = desktop->dt2doc(p_proj_on_cl - getSnapperTolerance() * direction_vector);    
    NR::Point const p_max_on_cl = desktop->dt2doc(p_proj_on_cl + getSnapperTolerance() * direction_vector);
    
    Geom::Path cl;
    cl.start(p_min_on_cl.to_2geom());
    cl.appendNew<Geom::LineSegment>(p_max_on_cl.to_2geom());
    
    for (std::vector<NArtBpath*>::const_iterator k = _bpaths_to_snap_to->begin(); k != _bpaths_to_snap_to->end(); k++) {
        if (*k) {                        
            // convert a Path object (see src/livarot/Path.h) to a 2geom's path object (see 2geom/path.h)
            // TODO: (Diederik) Only do this once for the first point, needs some storage of pointers in a member variable
            std::vector<Geom::Path> path_2geom = BPath_to_2GeomPath(*k);  
            
            for (std::vector<Geom::Path>::const_iterator l = path_2geom.begin(); l != path_2geom.end(); l++) {
                Geom::SimpleCrosser sxr;
                Geom::Crossings crossings = sxr.crossings(*l, cl);
                for (std::vector<Geom::Crossing>::const_iterator m = crossings.begin(); m != crossings.end(); m++) {
                    // Reconstruct the point of intersection
                    NR::Point p_inters = p_min_on_cl + ((*m).tb) * (p_max_on_cl - p_min_on_cl);
                    // When it's within snapping range, then return it
                    // (within snapping range == between p_min_on_cl and p_max_on_cl == 0 < tb < 1)
                    if ((*m).tb >= 0 && (*m).tb <= 1 ) {
                        NR::Coord dist = NR::L2(desktop->dt2doc(p_proj_on_cl) - p_inters);
                        SnappedPoint s(desktop->doc2dt(p_inters), dist, getSnapperTolerance(), getSnapperAlwaysSnap());
                        sc.points.push_back(s);    
                    }  
                } 
            }
        }
    }
}


void Inkscape::ObjectSnapper::_doFreeSnap(SnappedConstraints &sc,
                                            Inkscape::Snapper::PointType const &t,
                                            NR::Point const &p,
                                            bool const &first_point,
                                            std::vector<NR::Point> &points_to_snap,
                                            std::list<SPItem const *> const &it,
                                            std::vector<NR::Point> *unselected_nodes) const
{
    if ( NULL == _named_view ) {
        return;
    }

    /* Get a list of all the SPItems that we will try to snap to */
    if (first_point) {
        _findCandidates(sp_document_root(_named_view->document), it, first_point, points_to_snap, TRANSL_SNAP_XY);
    }
    
    if (_snap_to_itemnode || _snap_to_bboxnode) {
        _snapNodes(sc, t, p, first_point, unselected_nodes);
    }
    
    // Consider the page's border for snapping to
    NArtBpath *border_bpath = _snap_to_page_border ? _getBorderBPath() : NULL;   
    
    if (_snap_to_itempath || _snap_to_bboxpath || _snap_to_page_border) {
        unsigned n = (unselected_nodes == NULL) ? 0 : unselected_nodes->size();
        if (n > 0) {
            /* While editing a path in the node tool, findCandidates must ignore that path because 
             * of the node snapping requirements (i.e. only unselected nodes must be snapable).
             * That path must not be ignored however when snapping to the paths, so we add it here
             * manually when applicable
             */
            g_assert(it.size() == 1);
            g_assert(SP_IS_PATH(*it.begin()));
            _snapPaths(sc, t, p, first_point, unselected_nodes, SP_PATH(*it.begin()), border_bpath);
                
        } else {
            _snapPaths(sc, t, p, first_point, NULL, NULL, border_bpath);   
        }        
    }
}

void Inkscape::ObjectSnapper::_doConstrainedSnap( SnappedConstraints &sc,
                                                  Inkscape::Snapper::PointType const &t,
                                                  NR::Point const &p,
                                                  bool const &first_point,
                                                  std::vector<NR::Point> &points_to_snap,
                                                  ConstraintLine const &c,
                                                  std::list<SPItem const *> const &it) const
{
    if ( NULL == _named_view ) {
        return;
    }

    /* Get a list of all the SPItems that we will try to snap to */
    if (first_point) {
        _findCandidates(sp_document_root(_named_view->document), it, first_point, points_to_snap, TRANSL_SNAP_XY);
    }
    
    // A constrained snap, is a snap in only one degree of freedom (specified by the constraint line).
    // This is usefull for example when scaling an object while maintaining a fixed aspect ratio. It's
    // nodes are only allowed to move in one direction (i.e. in one degree of freedom).
    
    // When snapping to objects, we either snap to their nodes or their paths. It is however very
    // unlikely that any node will be exactly at the constrained line, so for a constrained snap
    // to objects we will only consider the object's paths. Beside, the nodes will be at these paths,
    // so we will more or less snap to them anyhow.   

    if (_snap_to_itempath || _snap_to_bboxpath || _snap_to_page_border) {
        _snapPathsConstrained(sc, t, p, first_point, c);
    }
}


// This method is used to snap a guide to nodes, while dragging the guide around
void Inkscape::ObjectSnapper::guideSnap(SnappedConstraints &sc,
										NR::Point const &p,
	                                    NR::Point const &guide_normal) const
{
    if ( NULL == _named_view ) {
        return;
    }
    
    /* Get a list of all the SPItems that we will try to snap to */
    std::vector<SPItem*> cand;
    std::list<SPItem const *> const it; //just an empty list

    std::vector<NR::Point> points_to_snap;
    points_to_snap.push_back(p);

    DimensionToSnap snap_dim;
    if (guide_normal == component_vectors[NR::Y]) {
        snap_dim = GUIDE_TRANSL_SNAP_Y;
    } else if (guide_normal == component_vectors[NR::X]) {
        snap_dim = GUIDE_TRANSL_SNAP_X;
    } else {
        snap_dim = ANGLED_GUIDE_TRANSL_SNAP;
    }
    // We don't support ANGLED_GUIDE_ROT_SNAP yet. 
    
    // It would be cool to allow the user to rotate a guide by dragging it, instead of
    // only translating it. (For example when CTRL is pressed). We will need an UI part 
    // for that first; and some important usability choices need to be made: 
    // E.g. which point should be used for pivoting? A previously snapped point,
    // or a transformation center (which can be moved after clicking for the
    // second time on an object; but should this point then be constrained to the
    // line, or can it be located anywhere?)

    _findCandidates(sp_document_root(_named_view->document), it, true, points_to_snap, snap_dim);
	_snapTranslatingGuideToNodes(sc, Inkscape::Snapper::SNAPPOINT_GUIDE, p, guide_normal);
    
    // _snapRotatingGuideToNodes has not been implemented yet. 
}

/**
 *  \return true if this Snapper will snap at least one kind of point.
 */
bool Inkscape::ObjectSnapper::ThisSnapperMightSnap() const
{
    bool snap_to_something = _snap_to_itempath || _snap_to_itemnode || _snap_to_bboxpath || _snap_to_bboxnode || _snap_to_page_border;
    return (_snap_enabled && _snap_from != 0 && snap_to_something);
}

bool Inkscape::ObjectSnapper::GuidesMightSnap() const
{
    bool snap_to_something = _snap_to_itemnode || _snap_to_bboxnode;
    return (_snap_enabled && (_snap_from & SNAPPOINT_GUIDE) && snap_to_something);
}

void Inkscape::ObjectSnapper::_clear_paths() const 
{
    for (std::vector<NArtBpath*>::const_iterator k = _bpaths_to_snap_to->begin(); k != _bpaths_to_snap_to->end(); k++) {
        g_free(*k);
    }
    _bpaths_to_snap_to->clear();
    
    for (std::vector<Path*>::const_iterator k = _paths_to_snap_to->begin(); k != _paths_to_snap_to->end(); k++) {
        delete *k;    
    }
    _paths_to_snap_to->clear();
}

NArtBpath* Inkscape::ObjectSnapper::_getBorderBPath() const
{
    NArtBpath *border_bpath = NULL;
    NR::Rect const border_rect = NR::Rect(NR::Point(0,0), NR::Point(sp_document_width(_named_view->document),sp_document_height(_named_view->document)));
    SPCurve const *border_curve = sp_curve_new_from_rect(border_rect);
    if (border_curve) {
        border_bpath = SP_CURVE_BPATH(border_curve); 
    }
        
    return border_bpath;
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
