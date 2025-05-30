/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 1999-2025 Vaclav Slavik
 *  Copyright (C) 2005 Olivier Sannier
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */

#include "edlistctrl.h"

#include "hidpi.h"
#include "language.h"
#include "cat_sorting.h"
#include "colorscheme.h"
#include "unicode_helpers.h"
#include "utility.h"

#include <wx/wx.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/wupdlock.h>

#ifdef __WXMSW__
#include <wx/headerctrl.h>
#include <wx/itemattr.h>
#include <wx/msw/uxtheme.h>
#include <vssym32.h>
#endif

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#include <algorithm>


namespace
{

class SelectionPreserver
{
public:
    SelectionPreserver(PoeditListCtrl *list_) : list(list_), focus(-1)
    {
        if (!list)
            return;
        selection = list->GetSelectedCatalogItemIndexes();
        focus = list->ListItemToCatalogIndex(list->GetCurrentItem());
    }

    ~SelectionPreserver()
    {
        if (!list)
            return;
        if (!selection.empty())
            list->SetSelectedCatalogItemIndexes(selection);
        if (focus != -1)
        {
            auto item = list->CatalogIndexToListItem(focus);
            list->EnsureVisible(item);
            list->SetCurrentItem(item);
        }
    }

private:
    PoeditListCtrl *list;
    std::vector<int> selection;
    int focus;
};


class DataViewMarkupRenderer : public wxDataViewTextRenderer
{
public:
    DataViewMarkupRenderer()
    {
        EnableMarkup();
        SetValueAdjuster(new Adjuster());
    }

    void SetHighlightedBgColor(const wxColour& bgHighlight)
    {
        static_cast<Adjuster*>(m_valueAdjuster)->SetHighlightedBgColor(bgHighlight);
    }

private:
    class Adjuster : public wxDataViewValueAdjuster
    {
    public:
        Adjuster() {}

        void SetHighlightedBgColor(const wxColour& bgHighlight)
        {
            m_bgHighlight = bgHighlight.GetAsString(wxC2S_HTML_SYNTAX);
        }

        wxVariant MakeHighlighted(const wxVariant& value) const override
        {
            auto s = value.GetString();
            bool changed = false;
            auto pos = s.find(" bgcolor=\"");
            if (pos != wxString::npos)
            {
                pos += 10;
                auto pos2 = s.find('"', pos);
                s.replace(pos, pos2 - pos, m_bgHighlight);
                changed = true;
            }
#ifdef __WXGTK__
            pos = s.find(" color=\"");
            if (pos != wxString::npos)
            {
                auto pos2 = s.find('"', pos + 8);
                s.erase(pos, pos2 - pos + 1);
                changed = true;
            }
#endif
            return changed ? wxVariant(s) : value;
        }

    private:
        wxString m_bgHighlight;
    };
};


/**
 * A wxDataViewColum whose GetWidth() prefers to return the fixed width over the actual width - they may differ, e.g.
 * for the last column which may automatically grow to take the available space on resize.
 */
class DataViewFixedColumn : public wxDataViewColumn
{

public:

    DataViewFixedColumn(const wxString &title, wxDataViewRenderer *renderer,
                       unsigned int model_column, int width = wxDVC_DEFAULT_WIDTH,
                       wxAlignment align = wxALIGN_CENTER) : wxDataViewColumn(title, renderer, model_column, width, align, 0)
    {
        fixed_width = width >= 0 ? width : wxCOL_WIDTH_DEFAULT;
    }

    /**
     * Returns the fixed width, if any, or the width returned by parent wxDataViewColumn.
     */
    int GetWidth() const override
    {
        if ( fixed_width != wxCOL_WIDTH_DEFAULT )
            return fixed_width;

        return wxDataViewColumn::GetWidth();
    }

    /**
     * Either:
     *
     * - Fixes the width if given width is >= 0
     * - Unsets the width if value is wxCOL_WIDTH_DEFAULT or wxCOL_WIDTH_AUTOSIZE;
     */
    void SetWidth( int width ) override
    {
        if ( width >= 0 )
        {
            fixed_width = width;
        }
        else if ( width == wxCOL_WIDTH_AUTOSIZE || width == wxCOL_WIDTH_DEFAULT )
        {
            fixed_width = wxCOL_WIDTH_DEFAULT;
        }

        wxDataViewColumn::SetWidth(width);
    }

private:
    // The fixed width, if any, or wxCOL_WIDTH_DEFAULT
    int fixed_width;
};

#if !defined(__WXMSW__) && !defined(__WXOSX__)

class DataViewIconsAdjuster : public wxDataViewValueAdjuster
{
public:
    DataViewIconsAdjuster()
    {
        m_comment = wxArtProvider::GetIcon("ItemCommentTemplate");
        m_commentSel = wxArtProvider::GetIcon("ItemCommentTemplate@inverted");
    }

    wxVariant MakeHighlighted(const wxVariant& value) const override
    {
        if (value.IsNull())
            return value;

        wxIcon icon;
        icon << value;

        if (icon.IsSameAs(m_comment))
        {
            wxVariant vout;
            vout << m_commentSel;
            return vout;
        }

        return value;
    }

private:
    wxIcon m_comment, m_commentSel;
};

#endif // !defined(__WXMSW__) && !defined(__WXOSX__)

wxString TrimTextValue(const wxString& text, size_t maxChars)
{
    wxString s(text.Strip(wxString::both));
    // FIXME: use syntax highlighting or typographic marks
    s.Replace("\n", " ");
    if (maxChars && s.length() > maxChars)
        return s.substr(0, maxChars);
    else
        return s;
}

} // anonymous namespace



PoeditListCtrl::Model::Model(TextDirection appTextDir)
    : m_frozen(false),
      m_maxVisibleWidth(0),
      m_sourceTextDir(TextDirection::LTR),
      m_transTextDir(TextDirection::LTR),
      m_appTextDir(appTextDir)
{
    sortOrder = SortOrder::Default();
}


void PoeditListCtrl::Model::SetVisualMode(ColorScheme::Mode visualMode)
{
    m_clrID = ColorScheme::Get(Color::ItemID, visualMode);
    m_clrFuzzy = ColorScheme::Get(Color::ItemFuzzy, visualMode);
    m_clrInvalid = ColorScheme::Get(Color::ItemError, visualMode);
    m_clrContextFg = ColorScheme::Get(Color::ItemContextFg, visualMode).GetAsString(wxC2S_HTML_SYNTAX);
    m_clrContextBg = ColorScheme::Get(Color::ItemContextBg, visualMode).GetAsString(wxC2S_HTML_SYNTAX);

    m_iconComment = wxArtProvider::GetIcon("ItemCommentTemplate");
    m_iconError = wxArtProvider::GetIcon("StatusError");
    m_iconWarning = wxArtProvider::GetIcon("StatusWarning");
}


void PoeditListCtrl::Model::SetCatalog(CatalogPtr catalog)
{
    m_catalog = catalog;

    if (!catalog)
    {
        Reset(0);
        m_mapListToCatalog.clear();
        m_mapCatalogToList.clear();
        return;
    }

    auto srclang = catalog->GetSourceLanguage();
    auto lang = catalog->GetLanguage();
    m_sourceTextDir = srclang.Direction();
    m_transTextDir = lang.Direction();

    // sort catalog items, create indexes mapping
    CreateSortMap();

    Reset(catalog->GetCount());
}


void PoeditListCtrl::Model::UpdateSort()
{
    if (!m_catalog)
        return;
    CreateSortMap();
    Reset(m_catalog->GetCount());
}


wxString PoeditListCtrl::Model::GetColumnType(unsigned int col) const
{
    switch (col)
    {
        case Col_ID:
            return "string";

        case Col_Icon:
            return "wxIcon";

        case Col_Source:
        case Col_Translation:
            return "string";

        default:
            return "null";
    }
}


void PoeditListCtrl::Model::GetValueByRow(wxVariant& variant, unsigned row, unsigned col) const
{
#if defined(__WXGTK__)
    #define NULL_ICON(variant)  variant << wxNullIcon
#else
    #define NULL_ICON(variant)  variant = wxNullVariant
#endif

    if (!m_catalog || m_frozen)
    {
#if defined(__WXGTK__)
        auto type = GetColumnType(col);
        if (type == "string")
            variant = "";
        else if (type == "wxIcon")
            NULL_ICON(variant);
        else
#else
        variant = wxNullVariant;
#endif
        if (col == Col_ID)
        {
            // sequential ID better than nothing, but this is a hack to get sizing correctly
            variant = wxString::Format("%d", (int)row);
        }
        return;
    }

    auto d = Item(row);
    wxCHECK_RET(d, "invalid row");

    switch (col)
    {
        case Col_ID:
        {
            variant = wxString::Format("%d", d->GetId());
            break;
        }

        case Col_Icon:
        {
            if (d->HasIssue())
            {
                switch (d->GetIssue()->severity)
                {
                    case CatalogItem::Issue::Error:
                        variant << m_iconError;
                        break;
                    case CatalogItem::Issue::Warning:
                        variant << m_iconWarning;
                        break;
                }
                break;
            }
            else if (d->HasComment())
                variant << m_iconComment;
            else
                NULL_ICON(variant);
            break;
        }

        case Col_Source:
        {
            wxString orig;
            const auto orig_str = TrimTextValue(d->GetString(), m_maxVisibleWidth);

        #ifdef __WXMSW__
            // Temporary workaround for https://github.com/vslavik/poedit/issues/343 and
            // https://github.com/vslavik/poedit/issues/481 -- fall back to old style rendering:
            if (m_appTextDir == TextDirection::RTL && m_sourceTextDir == TextDirection::LTR)
            {
                // non-markup rendering of source column:
                if (d->HasContext())
                    orig.Printf("[%s] %s", d->GetContext(), orig_str);
                else
                    orig = orig_str;
            }
            else
        #endif
            {
                if (d->HasContext())
                {
                    // Work around a problem with GTK+'s coloring of markup that begins with colorizing <span>:
                #ifdef __WXGTK__
                    #define MARKUP(x) L"\u200B" L##x
                #else
                    #define MARKUP(x) x
                #endif
                    orig.Printf(MARKUP("<span bgcolor=\"%s\" color=\"%s\"> %s </span> %s"),
                        m_clrContextBg, m_clrContextFg,
                        EscapeMarkup(d->GetContext()), EscapeMarkup(orig_str));
                }
                else
                {
                    orig = EscapeMarkup(orig_str);
                }
            }

            // Add RTL Unicode mark to render bidi texts correctly
            if (m_appTextDir != m_sourceTextDir)
                variant = bidi::mark_direction(orig, m_sourceTextDir);
            else
                variant = orig;
            break;
        }

        case Col_Translation:
        {
            const auto trans = TrimTextValue(d->GetTranslation(), m_maxVisibleWidth);

            // Add RTL Unicode mark to render bidi texts correctly
            if (m_appTextDir != m_transTextDir)
                variant = bidi::mark_direction(trans, m_transTextDir);
            else
                variant = trans;
            break;
        }

        default:
            variant.Clear();
            break;
    };
}

bool PoeditListCtrl::Model::SetValueByRow(const wxVariant&, unsigned, unsigned)
{
    wxFAIL_MSG("setting values in dataview not implemented");
    return false;
}

bool PoeditListCtrl::Model::GetAttrByRow(unsigned row, unsigned col, wxDataViewItemAttr& attr) const
{
    if (!m_catalog || m_frozen)
        return false;

    switch (col)
    {
        case Col_ID:
        {
            attr.SetColour(m_clrID);
            return true;
        }

        case Col_Source:
        case Col_Translation:
        {
            auto d = Item(row);
            if (d->HasError())
            {
                attr.SetColour(m_clrInvalid);
                return true;
            }
            else if (d->IsFuzzy())
            {
                attr.SetColour(m_clrFuzzy);
                return true;
            }
            else
            {
                return false;
            }
        }

        default:
            return false;
    }
}


void PoeditListCtrl::Model::CreateSortMap()
{
    // FIXME: Use native wxDataViewCtrl sorting instead

    int count = (int)m_catalog->GetCount();

    m_mapListToCatalog.resize(count);
    m_mapCatalogToList.resize(count);

    // First create identity mapping for the sort order.
    for ( int i = 0; i < count; i++ )
        m_mapListToCatalog[i] = i;

    // m_mapListToCatalog will hold our desired sort order. Sort it in place
    // now, using the desired sort criteria.
    CatalogItemsComparator comparator(*m_catalog, sortOrder);
    std::sort
    (
        m_mapListToCatalog.begin(),
        m_mapListToCatalog.end(),
        std::ref(comparator)
    );

    // Finally, construct m_mapCatalogToList to be the inverse mapping to
    // m_mapListToCatalog.
    for ( int i = 0; i < count; i++ )
        m_mapCatalogToList[m_mapListToCatalog[i]] = i;
}




PoeditListCtrl::PoeditListCtrl(wxWindow *parent, wxWindowID id, bool dispIDs)
     : wxDataViewCtrl(parent, id, wxDefaultPosition, wxDefaultSize, wxDV_MULTIPLE | wxDV_ROW_LINES | wxNO_BORDER, wxDefaultValidator, "translations list")
{
    m_displayIDs = dispIDs;
    m_appTextDir = (wxTheApp->GetLayoutDirection() == wxLayout_RightToLeft) ? TextDirection::RTL : TextDirection::LTR;

    m_model.reset(new Model(m_appTextDir));

    ColorScheme::SetupWindowColors(this, [=]
    {
        auto visualMode = ColorScheme::GetWindowMode(this);
        m_model->SetVisualMode(visualMode);
    #ifdef __WXMSW__
        if (visualMode == ColorScheme::Dark)
            SetAlternateRowColour(GetBackgroundColour().ChangeLightness(108));
        else
            SetAlternateRowColour(wxNullColour);
    #endif
    });

    AssociateModel(m_model.get());

    CreateColumns();
    UpdateColumns();

    UpdateHeaderAttrs();

#ifdef __WXMSW__
    GetMainWindow()->Bind(wxEVT_MENU, [=](wxCommandEvent&) {
        SelectAll();
        wxDataViewEvent le(wxEVT_DATAVIEW_SELECTION_CHANGED, this, GetSelection());
        ProcessWindowEvent(le); 
    }, wxID_SELECTALL);
    GetMainWindow()->Bind(wxEVT_UPDATE_UI, [=](wxUpdateUIEvent& e) {
        e.Enable(GetItemCount() > 0);
    }, wxID_SELECTALL);
#endif

    Bind(wxEVT_SIZE, &PoeditListCtrl::OnSize, this);
}

PoeditListCtrl::~PoeditListCtrl()
{
    sortOrder().Save();
}

void PoeditListCtrl::UpdateHeaderAttrs()
{
#ifdef __WXMSW__
    // Setup custom header font & color on Windows 10, where the default look is a bit odd
    
    if (wxUxThemeIsActive())
    {
        // Use the same text color as Explorer's headers use
        wxUxThemeHandle hTheme(this->GetParent(), L"ItemsView::Header");
        COLORREF clr;
        HRESULT hr = ::GetThemeColor(hTheme, HP_HEADERITEM, 0, TMT_TEXTCOLOR, &clr);
        if (SUCCEEDED(hr))
        {
            wxItemAttr headerAttr;
            headerAttr.SetTextColour(wxRGBToColour(clr));
            SetHeaderAttr(headerAttr);
        }
    }

    // Standard header has smaller height than Explorer's and it isn't
    // separated from the content well -- especially in HiDPI modes.
    // Match Explorer's header size too:
    int headerHeight = HiDPIScalingFactor() > 1.0 ? PX(26) : PX(25);
    GenericGetHeader()->SetMinSize(wxSize(-1, headerHeight));
#endif // __WXMSW__
}

void PoeditListCtrl::SetCustomFont(wxFont font_)
{
    wxFont font(font_);

    if ( !font.IsOk() )
        font = GetDefaultAttributes().font;

    SetFont(font);

#if defined(__WXOSX__)
    // Have to propagate font setting to native columns
    NSFont *nativeFont = font.OSXGetNSFont();
    NSTableView *tableView = (NSTableView*)[((NSScrollView*)GetHandle()) documentView];
    for (NSTableColumn *c in tableView.tableColumns)
        [c.dataCell setFont:nativeFont];

    // Custom setup of NSLayoutManager is necessary to match NSTableView sizing.
    // See http://stackoverflow.com/questions/17095927/dynamically-changing-row-height-after-font-size-of-entire-nstableview-nsoutlin
    NSLayoutManager *lm = [[NSLayoutManager alloc] init];
    [lm setTypesetterBehavior:NSTypesetterBehavior_10_2_WithCompatibility];
    CGFloat height = [lm defaultLineHeightForFont:nativeFont];
    SetRowHeight(int(height) + PX(4));
#elif defined(__WXMSW__)
    SetRowHeight(GetCharHeight() + PX(4));
#elif defined(__WXGTK__)
    // disable Ctrl+F in-control search:
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(GtkGetTreeView()), -1);
#endif

    UpdateHeaderAttrs();
    UpdateColumns();
}

void PoeditListCtrl::SetDisplayLines(bool dl)
{
    m_displayIDs = dl;
    UpdateColumns();
}

void PoeditListCtrl::CreateColumns()
{
    wxASSERT( GetColumnCount() == 0 );

#ifdef __WXOSX__
    NSTableView *tableView = (NSTableView*)[((NSScrollView*)GetHandle()) documentView];
    [tableView setIntercellSpacing:NSMakeSize(3.0, 0.0)];
    if (@available(macOS 11.0, *))
        tableView.style = NSTableViewStyleFullWidth;
#endif

    m_colID = m_colIcon = m_colSource = m_colTrans = nullptr;

#if defined(__WXMSW__)
    int iconWidth = wxArtProvider::GetBitmap("StatusError").GetSize().x + 6 /*wxDVC internal padding*/;
#else
    int iconWidth = PX(16);
#endif

    m_colIcon = AppendBitmapColumn(L"∙", Model::Col_Icon, wxDATAVIEW_CELL_INERT, iconWidth, wxALIGN_CENTER, 0);

    auto sourceRenderer = new DataViewMarkupRenderer();
    sourceRenderer->EnableEllipsize(wxELLIPSIZE_END);
    m_colSource = new wxDataViewColumn(_("Source text"), sourceRenderer, Model::Col_Source, wxCOL_WIDTH_DEFAULT, wxALIGN_LEFT, 0);
    AppendColumn(m_colSource);

    auto transRenderer = new wxDataViewTextRenderer();
    transRenderer->EnableEllipsize(wxELLIPSIZE_END);
    m_colTrans = new wxDataViewColumn(_(L"Translation — %s"), transRenderer, Model::Col_Translation, wxCOL_WIDTH_DEFAULT, wxALIGN_LEFT, 0);
    AppendColumn(m_colTrans);

    auto idRenderer = new wxDataViewTextRenderer();
    m_colID = new DataViewFixedColumn(_("ID"), idRenderer, Model::Col_ID, wxCOL_WIDTH_AUTOSIZE, wxALIGN_RIGHT);
    AppendColumn(m_colID);

    // wxDVC insists on having an expander column, but we really don't want one:
    auto fake = AppendTextColumn("", Model::Col_ID);
    fake->SetHidden(true);
    SetExpanderColumn(fake);

#ifdef __WXMSW__
    if (m_appTextDir == TextDirection::RTL)
        m_colSource->SetAlignment(wxALIGN_RIGHT);
#endif

    ColorScheme::SetupWindowColors(this, [=]
    {
    #if !defined(__WXMSW__) && !defined(__WXOSX__)
        if (ColorScheme::GetWindowMode(this) == ColorScheme::Light)
            m_colIcon->GetRenderer()->SetValueAdjuster(new DataViewIconsAdjuster());
        else
            m_colIcon->GetRenderer()->SetValueAdjuster(nullptr);
    #endif
        sourceRenderer->SetHighlightedBgColor(ColorScheme::Get(Color::ItemContextBgHighlighted, this));
    });
}

void PoeditListCtrl::UpdateColumns()
{
    wxASSERT( GetColumnCount() > 0 );

    if (!m_catalog)
        return;

    auto srclang = m_catalog->GetSourceLanguage();
    auto lang = m_catalog->GetLanguage();

    wxString sourceTitle;
    if (m_catalog->UsesSymbolicIDsForSource())
    {
        // TRANSLATORS: Refers to symbolic ID of source text, i.e. when something like "button.label" is used instead of English text
        sourceTitle = _("Source text ID");
    }
    else
    {
        sourceTitle = srclang.IsValid()
                      ? wxString::Format(_(L"Source text — %s"), srclang.DisplayName())
                      : _("Source text");
    }
    if (m_catalog->HasSideloadedReferenceFile())
    {
        wxFileName fn(m_catalog->GetSideloadedSourceData()->reference_file->GetFileName());
        fn.MakeRelativeTo(wxFileName(m_catalog->GetFileName()).GetPath());
        sourceTitle += wxString::Format(" [%s]", fn.GetFullPath());
    }
    m_colSource->SetTitle(sourceTitle);

#ifdef __WXMSW__
    // Temporary workaround for https://github.com/vslavik/poedit/issues/343 and
    // https://github.com/vslavik/poedit/issues/481 -- fall back to markup-less rendering
    // (see also above in PoeditListCtrl::Model::GetValueByRow):
    dynamic_cast<wxDataViewTextRenderer*>(m_colSource->GetRenderer())->EnableMarkup
    (
        m_appTextDir == TextDirection::LTR || srclang.IsRTL()
    );
#endif

    if (m_model->m_catalog && m_model->m_catalog->HasCapability(Catalog::Cap::Translations))
    {
        wxString langname = lang.IsValid() ? lang.DisplayName() : _("unknown language");;
        wxString transTitle = wxString::Format(_(L"Translation — %s"), langname);

        auto isRTL = lang.IsRTL();
#ifdef __WXMSW__
        // a quirk of wx API: if the current locale is RTL, the meaning of L and R is reversed
        if (m_appTextDir == TextDirection::RTL)
            isRTL = !isRTL;
#endif

        m_colTrans->SetHidden(false);
        m_colTrans->SetTitle(transTitle);
        m_colTrans->SetAlignment(isRTL ? wxALIGN_RIGHT : wxALIGN_LEFT);
    }
    else
    {
        m_colTrans->SetHidden(true);
    }

    m_colID->SetHidden(!m_displayIDs);

    // determine best fitting width only once, then set it as fixed, because IDs are immutable
    m_colID->SetWidth(wxCOL_WIDTH_AUTOSIZE);
    FixIdColumnSize();

    SizeColumns();

#ifdef __WXGTK__
    // wxGTK has delayed sizing computation, apparently
    CallAfter([=]{
        FixIdColumnSize();
        SizeColumns();
    });
#endif
}

void PoeditListCtrl::FixIdColumnSize()
{
    int computed_colID_width = m_colID->GetWidth();
    // The returned width may be 0 if it is not computed yet or if column is hidden - don't fix it yet in this case
    if ( computed_colID_width > 0 )
        m_colID->SetWidth(computed_colID_width);
}

void PoeditListCtrl::SizeColumns()
{
    int w = GetClientSize().x;

#if defined(__WXOSX__)
    {
        int visibleCols = 0;
        auto colCnt = GetColumnCount();
        for (auto c = 0; c < colCnt; c++)
        {
            if (!GetColumn(c)->IsHidden())
                visibleCols++;
        }
        w -= 3 * visibleCols;
        if (@available(macOS 11.0, *))
            w -= 9;
    }
#elif defined(__WXGTK__)
    w -= 2;
#endif

    if (m_colIcon)
        w -= m_colIcon->GetWidth();

    if (m_colID && m_colID->IsShown())
    {
        w -= m_colID->GetWidth();
#ifdef __WXGTK__
        w += 2;
#endif
    }

    // Tell the model to not bother with strings larger than twice available space:
    m_model->SetMaxVisibleWidth(w / GetCharWidth());

    if (m_colTrans && m_colTrans->IsShown())
    {
        m_colSource->SetWidth(w / 2);
        m_colTrans->SetWidth(w - w / 2);
    }
    else
    {
        m_colSource->SetWidth(w);
    }
}


void PoeditListCtrl::CatalogChanged(const CatalogPtr& catalog)
{
    if (!catalog)
    {
        m_catalog.reset();
        m_model->SetCatalog(nullptr);
        return;
    }

    wxWindowUpdateLocker no_updates(this);

    const int oldCount = m_model->GetCount();
    const int newCount = catalog ? catalog->GetCount() : 0;
    const bool isSameCatalog = (catalog == m_catalog);
    const bool sizeOrCatalogChanged = !isSameCatalog || (oldCount != newCount);

    SelectionPreserver preserve(!sizeOrCatalogChanged ? this : nullptr);

    // now read the new catalog:
    m_catalog = catalog;
    m_model->SetCatalog(catalog);

    UpdateColumns();

    if (sizeOrCatalogChanged && GetItemCount() > 0)
        CallAfter([=]{ SelectAndFocus(0); });
}


void PoeditListCtrl::RefreshAllItems()
{
    // Can't use Cleared() here because it messes up selection and scroll position and
    // implementations across platforms are inconsistent. Doing notification for all items
    // is reasonably fast except on macOS, where it is *extremely* inefficient and where
    // Cleared() messes up position. Fortunately, native reloadData is fast and equivalent
    // to reloading individual rows, so we can do just that.
#ifdef __WXOSX__
    NSTableView *tableView = (NSTableView*)[((NSScrollView*)GetHandle()) documentView];
    [tableView reloadData];
#else
    const int count = m_model->GetCount();
    wxDataViewItemArray items;
    items.reserve(count);
    for (int i = 0; i < count; i++)
        items.push_back(m_model->GetItem(i));
    m_model->ItemsChanged(items);
#endif
}


void PoeditListCtrl::Sort()
{
    if (!m_catalog)
        return;

    SelectionPreserver preserve(this);
    m_model->UpdateSort();
}


void PoeditListCtrl::OnSize(wxSizeEvent& event)
{
    wxWindowUpdateLocker lock(this);

    SizeColumns();
    event.Skip();
}

void PoeditListCtrl::DoFreeze()
{
    // FIXME: This is gross, but necessary if DVC is redrawn just between
    // changing m_catalog and calling on-changed notification, particularly when
    // updating from sources.
    //
    // Proper fix would be to either a) make a copy in cat_update.cpp instead of
    // updating a catalog in a way that may change its size or b) have
    // notifications integrated properly in Catalog and call them immediately
    // after a (size) change.
    m_model->Freeze();
    wxDataViewCtrl::DoFreeze();
}

void PoeditListCtrl::DoThaw()
{
    wxDataViewCtrl::DoThaw();
    m_model->Thaw();
}
