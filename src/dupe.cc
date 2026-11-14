/*
 * Copyright (C) 2005 John Ellis
 * Copyright (C) 2008 - 2016 The Geeqie Team
 *
 * Author: John Ellis
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "dupe.h"

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib-object.h>

#include "accelerators.h"
#include "actions.h"
#include "cache.h"
#include "collect.h"
#include "compat.h"
#include "date-time.h"
#include "dnd.h"
#include "filedata.h"
#include "history-list.h"
#include "image-load.h"
#include "img-view.h"
#include "intl.h"
#include "layout-image.h"
#include "layout-util.h"
#include "layout.h"
#include "main-defines.h"
#include "md5-util.h"
#include "menu.h"
#include "misc.h"
#include "options.h"
#include "pixbuf-util.h"
#include "print.h"
#include "similar.h"
#include "thumb.h"
#include "ui-file-chooser.h"
#include "ui-fileops.h"
#include "ui-menu.h"
#include "ui-misc.h"
#include "ui-utildlg.h"
#include "utilops.h"
#include "window.h"

using namespace std::string_literals;

namespace {

/** Columns displayed by the duplicate finder. */
enum {
	DUPE_COLUMN_RANK,
	DUPE_COLUMN_THUMB,
	DUPE_COLUMN_NAME,
	DUPE_COLUMN_SIZE,
	DUPE_COLUMN_DATE,
	DUPE_COLUMN_DIMENSIONS,
	DUPE_COLUMN_PATH,
	DUPE_COLUMN_SET
};

struct DupeRow
{
	GObject parent_instance;
	DupeItem *item;
	FileData *fd;
	GdkPixbuf *thumb;
	gchar *rank;
	GqSize dimensions;
	gint group;
	bool color;
};

struct DupeRowClass { GObjectClass parent_class; };

G_DEFINE_TYPE(DupeRow, dupe_row, G_TYPE_OBJECT)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(DupeRow, g_object_unref)

void dupe_row_finalize(GObject *object)
{
	auto *row = reinterpret_cast<DupeRow *>(object);
	file_data_unref(row->fd);
	g_clear_object(&row->thumb);
	g_free(row->rank);
	G_OBJECT_CLASS(dupe_row_parent_class)->finalize(object);
}

void dupe_row_class_init(DupeRowClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = dupe_row_finalize;
	g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
	             nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}

void dupe_row_init(DupeRow *) {}

DupeRow *dupe_row_new(DupeItem *item)
{
	auto *row = static_cast<DupeRow *>(g_object_new(dupe_row_get_type(), nullptr));
	row->item = item;
	row->fd = file_data_ref(item->fd);
	row->dimensions = item->dimensions;
	row->rank = g_strdup("");
	return row;
}

GtkSelectionModel *dupe_view_selection(GtkWidget *view)
{
	return gtk_column_view_get_model(GTK_COLUMN_VIEW(view));
}

GListStore *dupe_view_store(GtkWidget *view)
{
	return G_LIST_STORE(gtk_multi_selection_get_model(GTK_MULTI_SELECTION(dupe_view_selection(view))));
}

DupeRow *dupe_view_row(GtkWidget *view, guint position)
{
	return static_cast<DupeRow *>(g_list_model_get_item(G_LIST_MODEL(dupe_view_selection(view)), position));
}

gint dupe_listview_find_item(GtkWidget *view, DupeItem *item)
{
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(dupe_view_selection(view)));
	for (guint i = 0; i < count; ++i)
		{
		g_autoptr(DupeRow) row = dupe_view_row(view, i);
		if (row->item == item) return i;
		}
	return -1;
}

bool dupe_view_has_focus(GtkWidget *view)
{
	GtkRoot *root = gtk_widget_get_root(view);
	GtkWidget *focus = root ? gtk_root_get_focus(root) : nullptr;
	return focus && (focus == view || gtk_widget_is_ancestor(focus, view));
}

void dupe_view_select_item(GtkWidget *view, guint position, bool exclusive)
{
	gtk_selection_model_select_item(dupe_view_selection(view), position, exclusive);
	gtk_column_view_scroll_to(GTK_COLUMN_VIEW(view), position, nullptr,
	                          GTK_LIST_SCROLL_FOCUS, nullptr);
}

void dupe_view_remove(GtkWidget *view, guint position)
{
	auto *selection = dupe_view_selection(view);
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(selection));
	const bool selected = gtk_selection_model_is_selected(selection, position);
	g_list_store_remove(dupe_view_store(view), position);
	if (selected && count > 1)
		{
		const guint next = std::min(position, count - 2);
		gtk_selection_model_select_item(selection, next, FALSE);
		gtk_column_view_scroll_to(GTK_COLUMN_VIEW(view), next, nullptr, GTK_LIST_SCROLL_FOCUS, nullptr);
		}
}

gint dupe_view_compare(gconstpointer a, gconstpointer b, gpointer data)
{
	const auto *first = static_cast<const DupeRow *>(a);
	const auto *second = static_cast<const DupeRow *>(b);
	auto *sorter = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(data));
	auto *column = gtk_column_view_sorter_get_primary_sort_column(GTK_COLUMN_VIEW_SORTER(sorter));
	const bool sort_groups = column && GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "dupe-column")) == DUPE_COLUMN_SET;
	// Other columns sort within each group, retaining the discovery order.
	if (!sort_groups && first->group != second->group) return first->group < second->group ? 1 : -1;
	return gtk_sorter_compare(sorter, const_cast<DupeRow *>(first), const_cast<DupeRow *>(second));
}

void dupe_view_sort(GtkWidget *view)
{
	if (gtk_sorter_get_order(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(view))) == GTK_SORTER_ORDER_NONE) return;
	auto *selection = dupe_view_selection(view);
	g_autoptr(GHashTable) selected = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, nullptr);
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(selection));
	for (guint i = 0; i < count; ++i)
		{
		if (gtk_selection_model_is_selected(selection, i)) g_hash_table_add(selected, dupe_view_row(view, i));
		}
	g_list_store_sort(dupe_view_store(view), dupe_view_compare, view);
	gtk_selection_model_unselect_all(selection);
	for (guint i = 0; i < count; ++i)
		{
		g_autoptr(DupeRow) row = dupe_view_row(view, i);
		if (g_hash_table_contains(selected, row)) gtk_selection_model_select_item(selection, i, FALSE);
		}
}

guint dupe_view_position_at(GtkWidget *view, double x, double y)
{
	for (auto *widget = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT); widget && widget != view; widget = gtk_widget_get_parent(widget))
		{
		auto *item = static_cast<GtkListItem *>(g_object_get_data(G_OBJECT(widget), "dupe-list-item"));
		if (item) return gtk_list_item_get_position(item);
		}
	return GTK_INVALID_LIST_POSITION;
}

enum DUPE_CHECK_RESULT {
	DUPE_MATCH = 0,
	DUPE_NO_MATCH,
	DUPE_NAME_MATCH
};

/** @anchor dupe_queue_item
 * Used for similarity checks. One for each item pushed
 * onto the thread pool.
 */
struct DupeQueueItem
{
	DupeItem *needle;
	DupeWindow *dw;
	GList *work; /**< pointer into \a dw->list or \a dw->second_list (#DupeItem) */
	gint index; /**< The order items pushed onto thread pool. Used to sort returned matches */
};

/** @anchor dupe_search_match
 * Used for similarity checks thread. One for each pair match found.
 */
struct DupeSearchMatch
{
	DupeItem *a; /**< \a a / \a b matched pair found */
	DupeItem *b; /**< \a a / \a b matched pair found */
	gdouble rank;
	gint index; /**< The order items pushed onto thread pool. Used to sort returned matches */
};

constexpr gint DUPE_DEF_WIDTH = 800;
constexpr gint DUPE_DEF_HEIGHT = 400;

constexpr gdouble DUPE_PROGRESS_PULSE_STEP = 0.0001;

constexpr auto DUPE_WINDOW_DATA_KEY = "dupe-window";

DupeMatchType param_match_mask;
GList *dupe_window_list = nullptr;	/**< list of open DupeWindow *s */

} // namespace

DupeItem *dupe_listview_item_at(GtkWidget *view, guint position)
{
	g_autoptr(DupeRow) row = dupe_view_row(view, position);
	return row ? row->item : nullptr;
}

/*
 * Well, after adding the 'compare two sets' option things got a little sloppy in here
 * because we have to account for two 'modes' everywhere. (be careful).
 */

static void dupe_match_unlink(DupeItem *a, DupeItem *b);
static void dupe_window_recompare_idle(DupeWindow *dw);
static DupeItem *dupe_match_find_parent(DupeWindow *dw, DupeItem *child);

static gint dupe_match(DupeItem *a, DupeItem *b, DupeMatchType mask, gdouble *rank, gint fast);

static void dupe_thumb_step(DupeWindow *dw);
static gint dupe_check_cb(gpointer data);

static void dupe_second_add(DupeWindow *dw, DupeItem *di);
static void dupe_second_remove(DupeWindow *dw, DupeItem *di);
static void dupe_second_clear(DupeWindow *dw);
static GtkWidget *dupe_menu_popup_main(DupeWindow *dw);
static GtkWidget *dupe_menu_popup_second(DupeWindow *dw);

static void dupe_dnd_init(DupeWindow *dw);
static GdkContentProvider *dupe_dnd_prepare(GtkDragSource *source, gdouble x, gdouble y, gpointer data);
static void dupe_listview_press_cb(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data);
static void dupe_listview_release_cb(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data);


static void dupe_notify_cb(FileData *fd, NotifyType type, gpointer data);

static void dupe_init_list_cache(DupeWindow *dw);
static void dupe_destroy_list_cache(DupeWindow *dw);
static gboolean dupe_insert_in_list_cache(DupeWindow *dw, FileData *fd);

static void dupe_match_link(DupeItem *a, DupeItem *b, gdouble rank);
static gint dupe_match_link_exists(DupeItem *child, DupeItem *parent);

static bool dupe_pair_allowed(const DupeWindow *dw, const DupeItem *a, const DupeItem *b)
{
	if (!dw->ignore_same_folder) return true;
	const std::string_view first{a->fd->path};
	const std::string_view second{b->fd->path};
	return first.substr(0, first.find_last_of('/')) != second.substr(0, second.find_last_of('/'));
}

/**
 * @brief The function run in threads for similarity checks
 * @param d1 @ref dupe_queue_item "DupeQueueItem"
 * @param d2 #DupeWindow
 *
 * Used only for similarity checks.\n
 * Search \a dqi->list for \a dqi->needle and if a match is
 * found, create a @ref dupe_search_match "DupeSearchMatch" and add to \a dw->search_matches list\n
 * If \a dw->abort is set, just increment \a dw->thread_count
 */
static void dupe_comparison_func(gpointer d1, gpointer d2)
{
	auto dqi = static_cast<DupeQueueItem *>(d1);
	auto dw = static_cast<DupeWindow *>(d2);
	DupeSearchMatch *dsm;
	DupeItem *di;
	GList *matches = nullptr;
	gdouble rank = 0;

	if (!dw->abort)
		{
		GList *work = dqi->work;
		while (work)
			{
			di = static_cast<DupeItem *>(work->data);

			/* forward for second set, back for simple compare */
			if (dw->second_set)
				{
				work = work->next;
				}
			else
				{
				work = work->prev;
				}

			if (dupe_pair_allowed(dw, di, dqi->needle) &&
			    dupe_match(di, dqi->needle, dqi->dw->match_mask, &rank, TRUE))
				{
				dsm = g_new0(DupeSearchMatch, 1);
				dsm->a = di;
				dsm->b = dqi->needle;
				dsm->rank = rank;
				matches = g_list_prepend(matches, dsm);
				dsm->index = dqi->index;
				}

			if (dw->abort)
				{
				break;
				}
			}

		matches = g_list_reverse(matches);
		g_mutex_lock(&dw->search_matches_mutex);
		dw->search_matches = g_list_concat(dw->search_matches, matches);
		g_mutex_unlock(&dw->search_matches_mutex);
		}

	g_mutex_lock(&dw->thread_count_mutex);
	dw->thread_count++;
	g_mutex_unlock(&dw->thread_count_mutex);
	g_free(dqi);
}

/*
 * ------------------------------------------------------------------
 * Window updates
 * ------------------------------------------------------------------
 */

/**
 * @brief Update display of status label
 * @param dw
 * @param count_only
 *
 *
 */
static void dupe_window_update_count(DupeWindow *dw, gboolean count_only)
{
	g_autofree gchar *text = nullptr;

	if (!dw->list)
		{
		text = g_strdup(_("Drop files to compare them."));
		}
	else if (count_only)
		{
		text = g_strdup_printf(_("%d files"), g_list_length(dw->list));
		}
	else
		{
		text = g_strdup_printf(_("%d matches found in %d files"), g_list_length(dw->dupes), g_list_length(dw->list));
		}

	if (dw->second_set)
		{
		g_autofree gchar *buf = g_strconcat(text, " ", _("[set 1]"), NULL);
		std::swap(text, buf);
		}

	gtk_label_set_text(GTK_LABEL(dw->status_label), text);
}

/**
 * @brief Returns time in µsec since Epoch
 * @returns
 *
 *
 */
static guint64 msec_time()
{
	const auto duration = std::chrono::system_clock::now().time_since_epoch();

	return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

static gint dupe_iterations(gint n)
{
	return (n * ((n + 1) / 2));
}

/**
 * @brief
 * @param dw
 * @param status
 * @param value
 * @param force
 *
 * If \a status is blank, clear status bar text and set progress to zero. \n
 * If \a force is not set, after 2 secs has elapsed, update time-to-go every 250 ms.
 */
static void dupe_window_update_progress(DupeWindow *dw, const gchar *status, gdouble value, gboolean force)
{
	const gchar *status_text;

	if (status)
		{
		guint64 new_time = 0;

		if (dw->setup_n % 10 == 0)
			{
			new_time = msec_time() - dw->setup_time;
			}

		if (!force &&
		    value != 0.0 &&
		    dw->setup_count > 0 &&
		    new_time > 2000000)
			{
			gint t;
			gint d;
			guint32 rem;

			if (new_time - dw->setup_time_count < 250000) return;
			dw->setup_time_count = new_time;

			if (dw->setup_done)
				{
				if (dw->second_set)
					{
					t = dw->setup_count;
					d = dw->setup_count - dw->setup_n;
					}
				else
					{
					t = dupe_iterations(dw->setup_count);
					d = dupe_iterations(dw->setup_count - dw->setup_n);
					}
				}
			else
				{
				t = dw->setup_count;
				d = dw->setup_count - dw->setup_n;
				}

			rem = (t - d) ? (static_cast<gdouble>(dw->setup_time_count / 1000000.0) / (t - d)) * d : 0;

			gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dw->extra_label), value);

			g_autofree gchar *buf = g_strdup_printf("%s %d:%02d ", status, rem / 60, rem % 60);
			gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dw->extra_label), buf);

			return;
			}

		if (force ||
			 value == 0.0 ||
			 dw->setup_count == 0 ||
			 dw->setup_time_count == 0 ||
			 (new_time > 0 && new_time - dw->setup_time_count >= 250000))
			{
			if (dw->setup_time_count == 0) dw->setup_time_count = 1;
			if (new_time > 0) dw->setup_time_count = new_time;
			gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dw->extra_label), value);
			status_text = status;
			}
		else
			{
			status_text = nullptr;
			}
		}
	else
		{
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dw->extra_label), 0.0);
		status_text = " ";
		}

	if (status_text) gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dw->extra_label), status_text);
}

/*
 * ------------------------------------------------------------------
 * row color utils
 * ------------------------------------------------------------------
 */

static void dupe_listview_realign_colors(DupeWindow *dw)
{
	bool color = true;
	DupeItem *parent = nullptr;
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(dupe_view_store(dw->listview)));
	for (guint i = 0; i < count; ++i)
		{
		g_autoptr(DupeRow) row = dupe_view_row(dw->listview, i);
		auto *next_parent = dupe_match_find_parent(dw, row->item);
		if (!parent) color = row->color;
		else if (parent != next_parent) color = !color;
		parent = next_parent;
		row->color = color;
		g_signal_emit_by_name(row, "changed");
		}
}

/*
 * ------------------------------------------------------------------
 * Dupe item utils
 * ------------------------------------------------------------------
 */

static DupeItem *dupe_item_new(FileData *fd)
{
	auto *di = new DupeItem();

	di->fd = file_data_ref(fd);
	di->group_rank = 0.0;

	return di;
}

static void dupe_item_free(DupeItem *di)
{
	file_data_unref(di->fd);
	if (di->pixbuf) g_object_unref(di->pixbuf);

	delete di;
}

/*
 * ------------------------------------------------------------------
 * Image property cache
 * ------------------------------------------------------------------
 */

static void dupe_item_read_cache(DupeItem *di)
{
	if (!di) return;

	CacheData cd{};
	if (!cd.load(di->fd->path)) return;

	if (!di->simd && cd.similarity)
		{
		di->simd.swap(cd.similarity);
		}

	if (di->dimensions.empty() && cd.dimensions)
		{
		di->dimensions = cd.dimensions.value();
		di->dimensions_sum = (di->dimensions.width << 16) + di->dimensions.height;
		}

	if (!di->md5sum && cd.md5sum)
		{
		di->md5sum = md5_digest_to_text(cd.md5sum.value());
		}
}

static void dupe_item_write_cache(DupeItem *di)
{
	if (!di) return;

	CacheData cd{di->fd->path};

	if (!di->dimensions.empty()) cd.set_dimensions(di->dimensions);
	if (di->md5sum)
		{
		Md5Digest digest;
		if (md5_digest_from_text(di->md5sum->c_str(), digest)) cd.set_md5sum(digest);
		}
	if (di->simd) cd.set_similarity(*di->simd);

	cd.save(di->fd->path);
}

/*
 * ------------------------------------------------------------------
 * Window list utils
 * ------------------------------------------------------------------
 */

static void dupe_listview_add(DupeWindow *dw, DupeItem *parent, DupeItem *child)
{
	if (!parent) return;
	auto *store = dupe_view_store(dw->listview);
	gint position = 0;
	bool color = false;
	gint rank = 0;
	if (child)
		{
		position = dupe_listview_find_item(dw->listview, parent);
		if (position < 0) return;
		g_autoptr(DupeRow) parent_row = dupe_view_row(dw->listview, position);
		color = parent_row->color;
		++position;
		rank = child->group ? static_cast<gint>(floor(static_cast<DupeMatch *>(child->group->data)->rank)) : 1;
		}
	else if (g_list_model_get_n_items(G_LIST_MODEL(store)))
		{
		g_autoptr(DupeRow) first = dupe_view_row(dw->listview, 0);
		color = !first->color;
		++dw->set_count;
		}
	g_autoptr(DupeRow) row = dupe_row_new(child ? child : parent);
	row->color = color;
	row->group = dw->set_count;
	g_free(row->rank);
	if (!child && dw->second_set) row->rank = g_strdup("[1]");
	else if (!rank) row->rank = g_strdup(row->item->second ? "(2)" : "");
	else row->rank = g_strdup_printf("%d%s", rank, row->item->second ? " (2)" : "");
	g_list_store_insert(store, position, row);
}

static void dupe_listview_select_dupes(DupeWindow *dw, DupeSelectType parents);

static void dupe_listview_populate(DupeWindow *dw)
{
	auto *store = dupe_view_store(dw->listview);
	g_list_store_remove_all(store);

	for (GList *work = g_list_last(dw->dupes); work; work = work->prev)
		{
		auto parent = static_cast<DupeItem *>(work->data);

		dupe_listview_add(dw, parent, nullptr);

		for (GList *temp = g_list_last(parent->group); temp; temp = temp->prev)
			{
			auto dm = static_cast<DupeMatch *>(temp->data);

			dupe_listview_add(dw, parent, dm->di);
			}
		}

	dupe_view_sort(dw->listview);

	if (options->duplicates_select_type == DUPE_SELECT_GROUP1 ||
	    options->duplicates_select_type == DUPE_SELECT_GROUP2)
		{
		dupe_listview_select_dupes(dw, options->duplicates_select_type);
		}
}

static void dupe_listview_remove(DupeWindow *dw, DupeItem *di)
{
	if (!di) return;
	const gint position = dupe_listview_find_item(dw->listview, di);
	if (position < 0) return;
	dupe_view_remove(dw->listview, position);
	if (g_list_find(dw->dupes, di) && !dw->color_frozen) dupe_listview_realign_colors(dw);
}

static GList *dupe_listview_get_selection(GtkWidget *view)
{
	GList *files = nullptr;
	auto *selection = dupe_view_selection(view);
	g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(selection);
	GtkBitsetIter iter;
	guint position;
	for (bool valid = gtk_bitset_iter_init_first(&iter, selected, &position); valid; valid = gtk_bitset_iter_next(&iter, &position))
		{
		g_autoptr(DupeRow) row = dupe_view_row(view, position);
		if (!g_list_find(files, row->fd)) files = g_list_prepend(files, file_data_ref(row->fd));
		}
	return g_list_reverse(files);
}

static gboolean dupe_listview_item_is_selected(DupeItem *item, GtkWidget *view)
{
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(dupe_view_store(view)));
	for (guint i = 0; i < count; ++i)
		{
		if (dupe_listview_item_at(view, i) == item && gtk_selection_model_is_selected(dupe_view_selection(view), i)) return TRUE;
		}
	return FALSE;
}

static void dupe_listview_select_dupes(DupeWindow *dw, DupeSelectType parents)
{
	auto *selection = dupe_view_selection(dw->listview);
	gtk_selection_model_unselect_all(selection);
	gint last_group = -1;
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(selection));
	for (guint i = 0; i < count; ++i)
		{
		g_autoptr(DupeRow) row = dupe_view_row(dw->listview, i);
		const bool first = row->group != last_group;
		if ((first && parents == DUPE_SELECT_GROUP1) || (!first && parents == DUPE_SELECT_GROUP2))
			gtk_selection_model_select_item(selection, i, FALSE);
		last_group = row->group;
		}
}

/*
 * ------------------------------------------------------------------
 * Match group manipulation
 * ------------------------------------------------------------------
 */

/**
 * @brief Search \a parent->group for \a child (#DupeItem)
 * @param child
 * @param parent
 * @returns
 *
 */
static DupeMatch *dupe_match_find_match(DupeItem *child, DupeItem *parent)
{
	GList *work;

	work = parent->group;
	while (work)
		{
		auto dm = static_cast<DupeMatch *>(work->data);
		if (dm->di == child) return dm;
		work = work->next;
		}
	return nullptr;
}

/**
 * @brief Create #DupeMatch structure for \a child, and insert into \a parent->group list.
 * @param child
 * @param parent
 * @param rank
 *
 */
static void dupe_match_link_child(DupeItem *child, DupeItem *parent, gdouble rank)
{
	DupeMatch *dm;

	dm = g_new0(DupeMatch, 1);
	dm->di = child;
	dm->rank = rank;
	parent->group = g_list_append(parent->group, dm);
}

/**
 * @brief Link \a a & \a b as both parent and child
 * @param a
 * @param b
 * @param rank
 *
 * Link \a a as child of \a b, and \a b as child of \a a
 */
static void dupe_match_link(DupeItem *a, DupeItem *b, gdouble rank)
{
	dupe_match_link_child(a, b, rank);
	dupe_match_link_child(b, a, rank);
}

/**
 * @brief Remove \a child #DupeMatch from \a parent->group list.
 * @param child
 * @param parent
 *
 */
static void dupe_match_unlink_child(DupeItem *child, DupeItem *parent)
{
	DupeMatch *dm;

	dm = dupe_match_find_match(child, parent);
	if (dm)
		{
		parent->group = g_list_remove(parent->group, dm);
		g_free(dm);
		}
}

/**
 * @brief  Unlink \a a from \a b, and \a b from \a a
 * @param a
 * @param b
 *
 * Free the relevant #DupeMatch items from the #DupeItem group lists
 */
static void dupe_match_unlink(DupeItem *a, DupeItem *b)
{
	dupe_match_unlink_child(a, b);
	dupe_match_unlink_child(b, a);
}

/**
 * @brief
 * @param parent
 * @param unlink_children
 *
 * If \a unlink_children is set, unlink all entries in \a parent->group list. \n
 * Free the \a parent->group list and set group_rank to zero;
 */
static void dupe_match_link_clear(DupeItem *parent, gboolean unlink_children)
{
	if (unlink_children)
		{
		GList *work;

		work = parent->group;
		while (work)
			{
			auto dm = static_cast<DupeMatch *>(work->data);
			work = work->next;

			dupe_match_unlink_child(parent, dm->di);
			}
		}

	g_list_free_full(parent->group, g_free);
	parent->group = nullptr;
	parent->group_rank = 0.0;
}

/**
 * @brief Search \a parent->group list for \a child
 * @param child
 * @param parent
 * @returns boolean TRUE/FALSE found/not found
 *
 */
static gint dupe_match_link_exists(DupeItem *child, DupeItem *parent)
{
	return (dupe_match_find_match(child, parent) != nullptr);
}

/**
 * @brief  Search \a parent->group for \a child, and return \a child->rank
 * @param child
 * @param parent
 * @returns \a dm->di->rank
 *
 */
static gdouble dupe_match_link_rank(DupeItem *child, DupeItem *parent)
{
	DupeMatch *dm;

	dm = dupe_match_find_match(child, parent);
	if (dm) return dm->rank;

	return 0.0;
}

/**
 * @brief Find highest rank in \a child->group
 * @param child
 * @returns
 *
 * Search the #DupeMatch entries in the \a child->group list.
 * Return the #DupeItem with the highest rank. If more than one have
 * the same rank, the first encountered is used.
 */
static DupeItem *dupe_match_highest_rank(DupeItem *child)
{
	DupeMatch *dr;
	GList *work;

	dr = nullptr;
	work = child->group;
	while (work)
		{
		auto dm = static_cast<DupeMatch *>(work->data);
		if (!dr || dm->rank > dr->rank)
			{
			dr = dm;
			}
		work = work->next;
		}

	return (dr) ? dr->di : nullptr;
}

/**
 * @brief Compute and store \a parent->group_rank
 * @param parent
 *
 * Group_rank = (sum of all child ranks) / n
 */
static void dupe_match_rank_update(DupeItem *parent)
{
	GList *work;
	gdouble rank = 0.0;
	gint c = 0;

	work = parent->group;
	while (work)
		{
		auto dm = static_cast<DupeMatch *>(work->data);
		work = work->next;
		rank += dm->rank;
		c++;
		}

	if (c > 0)
		{
		parent->group_rank = rank / c;
		}
	else
		{
		parent->group_rank = 0.0;
		}
}

static DupeItem *dupe_match_find_parent(DupeWindow *dw, DupeItem *child)
{
	GList *work;

	if (g_list_find(dw->dupes, child)) return child;

	work = child->group;
	while (work)
		{
		auto dm = static_cast<DupeMatch *>(work->data);
		if (g_list_find(dw->dupes, dm->di)) return dm->di;
		work = work->next;
		}

	return nullptr;
}

/**
 * @brief
 * @param work (#DupeItem) dw->list or dw->second_list
 *
 * Unlink all #DupeItem-s in \a work.
 * Do not unlink children.
 */
static void dupe_match_reset_list(GList *work)
{
	while (work)
		{
		auto di = static_cast<DupeItem *>(work->data);
		work = work->next;

		dupe_match_link_clear(di, FALSE);
		}
}

static void dupe_match_reparent(DupeWindow *dw, DupeItem *old_parent, DupeItem *new_parent)
{
	GList *work;

	if (!old_parent || !new_parent || !dupe_match_link_exists(old_parent, new_parent)) return;

	dupe_match_link_clear(new_parent, TRUE);
	work = old_parent->group;
	while (work)
		{
		auto dm = static_cast<DupeMatch *>(work->data);
		dupe_match_unlink_child(old_parent, dm->di);
		dupe_match_link_child(new_parent, dm->di, dm->rank);
		work = work->next;
		}

	new_parent->group = old_parent->group;
	old_parent->group = nullptr;

	work = g_list_find(dw->dupes, old_parent);
	if (work) work->data = new_parent;
}

static void dupe_match_print_group(DupeItem *di)
{
	GList *work;

	log_printf("+ %f %s\n", di->group_rank, di->fd->name);

	work = di->group;
	while (work)
		{
		auto dm = static_cast<DupeMatch *>(work->data);
		work = work->next;

		log_printf("  %f %s\n", dm->rank, dm->di->fd->name);
		}

	log_printf("\n");
}

static void dupe_match_print_list(GList *list)
{
	GList *work;

	work = list;
	while (work)
		{
		auto di = static_cast<DupeItem *>(work->data);
		dupe_match_print_group(di);
		work = work->next;
		}
}

/* level 3, unlinking and orphan handling */
/**
 * @brief
 * @param child
 * @param parent \a di from \a child->group
 * @param[inout] list \a dw->list sorted by rank (#DupeItem)
 * @param dw
 * @returns modified \a list
 *
 * Called for each entry in \a child->group (#DupeMatch) with \a parent set to \a dm->di. \n
 * Find the highest rank #DupeItem of the \a parent's children. \n
 * If that is == \a child OR
 * highest rank #DupeItem of \a child == \a parent then FIXME:
 *
 */
static GList *dupe_match_unlink_by_rank(DupeItem *child, DupeItem *parent, GList *list, DupeWindow *dw)
{
	DupeItem *best = nullptr;

	best = dupe_match_highest_rank(parent); // highest rank in parent->group
	if (best == child || dupe_match_highest_rank(child) == parent)
		{
		GList *work;
		gdouble rank;

		DEBUG_2("link found %s to %s [%d]", child->fd->name, parent->fd->name, g_list_length(parent->group));

		work = parent->group;
		while (work)
			{
			auto dm = static_cast<DupeMatch *>(work->data);
			DupeItem *orphan;

			work = work->next;
			orphan = dm->di;
			if (orphan != child && g_list_length(orphan->group) < 2)
				{
				dupe_match_link_clear(orphan, TRUE);
				if (!dw->second_set || orphan->second)
					{
					dupe_match(orphan, child, dw->match_mask, &rank, FALSE);
					dupe_match_link(orphan, child, rank);
					}
				list = g_list_remove(list, orphan);
				}
			}

		rank = dupe_match_link_rank(child, parent); // child->rank
		dupe_match_link_clear(parent, TRUE);
		dupe_match_link(child, parent, rank);
		list = g_list_remove(list, parent);
		}
	else
		{
		DEBUG_2("unlinking %s and %s", child->fd->name, parent->fd->name);

		dupe_match_unlink(child, parent);
		}

	return list;
}

/* level 2 */
/**
 * @brief
 * @param[inout] list \a dw->list sorted by rank (#DupeItem)
 * @param di
 * @param dw
 * @returns modified \a list
 *
 * Called for each entry in \a list.
 * Call unlink for each child in \a di->group
 */
static GList *dupe_match_group_filter(GList *list, DupeItem *di, DupeWindow *dw)
{
	GList *work;

	work = g_list_last(di->group);
	while (work)
		{
		auto dm = static_cast<DupeMatch *>(work->data);
		work = work->prev;
		list = dupe_match_unlink_by_rank(di, dm->di, list, dw);
		}

	return list;
}

/* level 1 (top) */
/**
 * @brief
 * @param[inout] list \a dw->list sorted by rank (#DupeItem)
 * @param dw
 * @returns Filtered \a list
 *
 * Called once.
 * Call group filter for each \a di in \a list
 */
static GList *dupe_match_group_trim(GList *list, DupeWindow *dw)
{
	GList *work;

	work = list;
	while (work)
		{
		auto di = static_cast<DupeItem *>(work->data);
		if (!di->second) list = dupe_match_group_filter(list, di, dw);
		work = work->next;
		if (di->second) list = g_list_remove(list, di);
		}

	return list;
}

static gint dupe_match_sort_groups_cb(gconstpointer a, gconstpointer b)
{
	auto da = static_cast<const DupeMatch *>(a);
	auto db = static_cast<const DupeMatch *>(b);

	if (da->rank > db->rank) return -1;
	if (da->rank < db->rank) return 1;
	return 0;
}

/**
 * @brief Sorts the children of each #DupeItem in \a list
 * @param list #DupeItem
 *
 * Sorts the #DupeItem->group children on rank
 */
static void dupe_match_sort_groups(GList *list)
{
	GList *work;

	work = list;
	while (work)
		{
		auto di = static_cast<DupeItem *>(work->data);
		di->group = g_list_sort(di->group, dupe_match_sort_groups_cb);
		work = work->next;
		}
}

static gint dupe_match_totals_sort_cb(gconstpointer a, gconstpointer b)
{
	auto da = static_cast<const DupeItem *>(a);
	auto db = static_cast<const DupeItem *>(b);

	if (g_list_length(da->group) > g_list_length(db->group)) return -1;
	if (g_list_length(da->group) < g_list_length(db->group)) return 1;

	if (da->group_rank < db->group_rank) return -1;
	if (da->group_rank > db->group_rank) return 1;

	return 0;
}

/**
 * @brief Callback for group_rank sort
 * @param a
 * @param b
 * @returns
 *
 *
 */
static gint dupe_match_rank_sort_cb(gconstpointer a, gconstpointer b)
{
	auto da = static_cast<const DupeItem *>(a);
	auto db = static_cast<const DupeItem *>(b);

	if (da->group_rank > db->group_rank) return -1;
	if (da->group_rank < db->group_rank) return 1;
	return 0;
}

/**
 * @brief Sorts \a source_list by group-rank
 * @param source_list #DupeItem
 * @returns
 *
 * Computes group_rank for each #DupeItem. \n
 * Items with no group list are ignored.
 * Returns allocated GList of #DupeItem-s sorted by group_rank
 */
static GList *dupe_match_rank_sort(GList *source_list)
{
	GList *list = nullptr;
	GList *work;

	work = source_list;
	while (work)
		{
		auto di = static_cast<DupeItem *>(work->data);

		if (di->group)
			{
			dupe_match_rank_update(di); // Compute and store group_rank for di
			list = g_list_prepend(list, di);
			}

		work = work->next;
		}

	return g_list_sort(list, dupe_match_rank_sort_cb);
}

/**
 * @brief Returns allocated GList of dupes sorted by totals
 * @param source_list
 * @returns
 *
 *
 */
static GList *dupe_match_totals_sort(GList *source_list)
{
	source_list = g_list_sort(source_list, dupe_match_totals_sort_cb);

	source_list = g_list_first(source_list);
	return g_list_reverse(source_list);
}

/* Cover matched files with existing neighborhoods. Unlike the usual grouping
 * heuristic, this never invents links between members of different groups.
 * A member can appear in more than one group when needed to retain matches.
 */
static GList *dupe_match_group_cover(GList *list)
{
	g_autoptr(GHashTable) covered = g_hash_table_new(g_direct_hash, g_direct_equal);
	GList *groups = nullptr;
	while (true)
		{
		DupeItem *best = nullptr;
		guint best_count = 0;
		for (GList *work = list; work; work = work->next)
			{
			auto *di = static_cast<DupeItem *>(work->data);
			// Keep representatives out of other groups' member lists, so selecting
			// group 2 duplicates always leaves every representative unselected.
			if (g_hash_table_contains(covered, di)) continue;
			guint count = 1;
			for (GList *member = di->group; member; member = member->next)
				{
				auto *match = static_cast<DupeMatch *>(member->data);
				if (!g_hash_table_contains(covered, match->di)) ++count;
				}
			if (count > best_count)
				{
				best = di;
				best_count = count;
				}
			}
		if (!best) break;
		groups = g_list_prepend(groups, best);
		g_hash_table_add(covered, best);
		for (GList *member = best->group; member; member = member->next)
			{
			auto *match = static_cast<DupeMatch *>(member->data);
			g_hash_table_add(covered, match->di);
			}
		}
	g_list_free(list);
	return g_list_reverse(groups);
}

static void dupe_match_rank(DupeWindow *dw)
{
	GList *list;

	list = dupe_match_rank_sort(dw->list); // sorted by group_rank, no-matches filtered out

	if (required_debug_level(2)) dupe_match_print_list(list);

	DEBUG_1("Similar items: %u", g_list_length(list)); // NOTRANSLATE: Diagnostic message.
	list = dw->ignore_same_folder ? dupe_match_group_cover(list) : dupe_match_group_trim(list, dw);
	DEBUG_1("Unique groups: %u", g_list_length(list)); // NOTRANSLATE: Diagnostic message.

	dupe_match_sort_groups(list);

	if (required_debug_level(2)) dupe_match_print_list(list);

	list = dupe_match_rank_sort(list);
	if (options->sort_totals)
		{
		list = dupe_match_totals_sort(list);
		}
	if (required_debug_level(2)) dupe_match_print_list(list);

	g_list_free(dw->dupes);
	dw->dupes = list;
}

/*
 * ------------------------------------------------------------------
 * Match group tests
 * ------------------------------------------------------------------
 */

static gboolean dupe_match_md5sum(DupeItem *a, DupeItem *b)
{
	if (!a->md5sum) a->md5sum = md5_text_from_file_utf8(a->fd->path);
	if (!b->md5sum) b->md5sum = md5_text_from_file_utf8(b->fd->path);

	return !a->md5sum->empty()
	    && !b->md5sum->empty()
	    && a->md5sum == b->md5sum;
}

/**
 * @brief
 * @param[in] a
 * @param[in] b
 * @param[in] mask
 * @param[out] rank
 * @param[in] fast
 * @returns
 *
 * For similarity checks, compute rank - (similarity factor between a and b). \n
 * If rank < user-set sim value, returns FALSE.
 */
static gboolean dupe_match(DupeItem *a, DupeItem *b, DupeMatchType mask, gdouble *rank, gint fast)
{
	*rank = 0.0;

	if (a->fd->path == b->fd->path) return FALSE;

	if (mask & DUPE_MATCH_ALL)
		{
		return TRUE;
		}
	if (mask & DUPE_MATCH_PATH)
		{
		if (utf8_compare(a->fd->path, b->fd->path, TRUE) != 0) return FALSE;
		}
	if (mask & DUPE_MATCH_NAME)
		{
		if (strcmp(a->fd->collate_key_name, b->fd->collate_key_name) != 0) return FALSE;
		}
	if (mask & DUPE_MATCH_NAME_CI)
		{
		if (strcmp(a->fd->collate_key_name_nocase, b->fd->collate_key_name_nocase) != 0) return FALSE;
		}
	if (mask & DUPE_MATCH_NAME_CONTENT)
		{
		if (strcmp(a->fd->collate_key_name, b->fd->collate_key_name) != 0) return FALSE;

		return !dupe_match_md5sum(a, b);
		}
	if (mask & DUPE_MATCH_NAME_CI_CONTENT)
		{
		if (strcmp(a->fd->collate_key_name_nocase, b->fd->collate_key_name_nocase) != 0) return FALSE;

		return !dupe_match_md5sum(a, b);
		}
	if (mask & DUPE_MATCH_SIZE)
		{
		if (a->fd->size != b->fd->size) return FALSE;
		}
	if (mask & DUPE_MATCH_DATE)
		{
		if (a->fd->date != b->fd->date) return FALSE;
		}
	if (mask & DUPE_MATCH_SUM)
		{
		if (!dupe_match_md5sum(a, b)) return FALSE;
		}
	if (mask & DUPE_MATCH_DIM)
		{
		if (a->dimensions.empty()) image_load_dimensions(a->fd, a->dimensions);
		if (b->dimensions.empty()) image_load_dimensions(b->fd, b->dimensions);
		if (a->dimensions != b->dimensions) return FALSE;
		}
	if (mask & DUPE_MATCH_SIM)
		{
		gdouble f;
		gdouble m;

		if (mask & DUPE_MATCH_SIM_HIGH) m = 0.95;
		else if (mask & DUPE_MATCH_SIM_MED) m = 0.90;
		else if (mask & DUPE_MATCH_SIM_CUSTOM) m = static_cast<gdouble>(options->duplicates_similarity_threshold) / 100.0;
		else m = 0.85;

		if (fast)
			{
			f = image_sim_compare_fast(a->simd.get(), b->simd.get(), m);
			}
		else
			{
			f = image_sim_compare(a->simd.get(), b->simd.get());
			}

		*rank = f * 100.0;

		if (f < m) return FALSE;

		DEBUG_3("similar: %32s %32s = %f", a->fd->name, b->fd->name, f);
		}

	return TRUE;
}

/**
 * @brief  Determine if there is a match
 * @param di1
 * @param di2
 * @param data
 * @returns DUPE_MATCH/DUPE_NO_MATCH/DUPE_NAME_MATCH
 * 			DUPE_NAME_MATCH is used for name != contents searches:
 * 							the name and content match i.e.
 * 							no match, but keep searching
 *
 * Called when stepping down the array looking for adjacent matches,
 * and from the 2nd set search.
 *
 * Is not used for similarity checks.
 */
static DUPE_CHECK_RESULT dupe_match_check(DupeItem *di1, DupeItem *di2, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);
	DupeMatchType mask = dw->match_mask;

	if (mask & DUPE_MATCH_ALL)
		{
		return DUPE_MATCH;
		}
	if (mask & DUPE_MATCH_PATH)
		{
		if (utf8_compare(di1->fd->path, di2->fd->path, TRUE) != 0)
			{
			return DUPE_NO_MATCH;
			}
		}
	if (mask & DUPE_MATCH_NAME)
		{
		if (g_strcmp0(di1->fd->collate_key_name, di2->fd->collate_key_name) != 0)
			{
			return DUPE_NO_MATCH;
			}
		}
	if (mask & DUPE_MATCH_NAME_CI)
		{
		if (g_strcmp0(di1->fd->collate_key_name_nocase, di2->fd->collate_key_name_nocase) != 0 )
			{
			return DUPE_NO_MATCH;
			}
		}
	if (mask & DUPE_MATCH_NAME_CONTENT)
		{
		if (g_strcmp0(di1->fd->collate_key_name, di2->fd->collate_key_name) != 0)
			{
			return DUPE_NO_MATCH;
			}

		if (di1->md5sum == di2->md5sum)
			{
			return DUPE_NAME_MATCH;
			}
		}
	if (mask & DUPE_MATCH_NAME_CI_CONTENT)
		{
		if (strcmp(di1->fd->collate_key_name_nocase, di2->fd->collate_key_name_nocase) != 0)
			{
			return DUPE_NO_MATCH;
			}

		if (di1->md5sum == di2->md5sum)
			{
			return DUPE_NAME_MATCH;
			}
		}
	if (mask & DUPE_MATCH_SIZE)
		{
		if (di1->fd->size != di2->fd->size)
			{
			return DUPE_NO_MATCH;
			}
		}
	if (mask & DUPE_MATCH_DATE)
		{
		if (di1->fd->date != di2->fd->date)
			{
			return DUPE_NO_MATCH;
			}
		}
	if (mask & DUPE_MATCH_SUM)
		{
		if (di1->md5sum != di2->md5sum)
			{
			return DUPE_NO_MATCH;
			}
		}
	if (mask & DUPE_MATCH_DIM)
		{
		if (di1->dimensions_sum != di2->dimensions_sum)
			{
			return DUPE_NO_MATCH;
			}
		}

	return DUPE_MATCH;
}

/**
 * @brief The callback for the binary search
 * @param a
 * @param b
 * @returns negative/0/positive
 *
 * Is not used for similarity checks.
 *
 * Used only when two file sets are used.
 * Requires use of a global for param_match_mask because there is no
 * g_array_binary_search_with_data() function in glib.
 */
static gint dupe_match_binary_search_cb(gconstpointer a, gconstpointer b)
{
	auto di1 = *(static_cast<const DupeItem *const *>(a));
	auto di2 = static_cast<const DupeItem *>(b);
	DupeMatchType mask = param_match_mask;

	if (mask & DUPE_MATCH_ALL)
		{
		return 0;
		}
	if (mask & DUPE_MATCH_PATH)
		{
		return utf8_compare(di1->fd->path, di2->fd->path, TRUE);
		}
	if (mask & DUPE_MATCH_NAME)
		{
		return g_strcmp0(di1->fd->collate_key_name, di2->fd->collate_key_name);
		}
	if (mask & DUPE_MATCH_NAME_CI)
		{
		return strcmp(di1->fd->collate_key_name_nocase, di2->fd->collate_key_name_nocase);
		}
	if (mask & DUPE_MATCH_NAME_CONTENT)
		{
		return g_strcmp0(di1->fd->collate_key_name, di2->fd->collate_key_name);
		}
	if (mask & DUPE_MATCH_NAME_CI_CONTENT)
		{
		return strcmp(di1->fd->collate_key_name_nocase, di2->fd->collate_key_name_nocase);
		}
	if (mask & DUPE_MATCH_SIZE)
		{
		return (di1->fd->size - di2->fd->size);
		}
	if (mask & DUPE_MATCH_DATE)
		{
		return (di1->fd->date - di2->fd->date);
		}
	if (mask & DUPE_MATCH_SUM)
		{
		if (di1->md5sum < di2->md5sum) return -1;
		if (di1->md5sum > di2->md5sum) return 1;
		return 0;
		}
	if (mask & DUPE_MATCH_DIM)
		{
		return (di1->dimensions_sum - di2->dimensions_sum);
		}

	return 0;
}

/**
 * @brief The callback for the array sort
 * @param a
 * @param b
 * @param data
 * @returns negative/0/positive
 *
 * Is not used for similarity checks.
*/
static gint dupe_match_sort_cb(gconstpointer a, gconstpointer b, gpointer data)
{
	auto di1 = *(static_cast<const DupeItem *const *>(a));
	auto di2 = *(static_cast<const DupeItem *const *>(b));
	auto dw = static_cast<DupeWindow *>(data);
	DupeMatchType mask = dw->match_mask;

	if (mask & DUPE_MATCH_ALL)
		{
		return 0;
		}
	if (mask & DUPE_MATCH_PATH)
		{
		return utf8_compare(di1->fd->path, di2->fd->path, TRUE);
		}
	if (mask & DUPE_MATCH_NAME)
		{
		return g_strcmp0(di1->fd->collate_key_name, di2->fd->collate_key_name);
		}
	if (mask & DUPE_MATCH_NAME_CI)
		{
		return strcmp(di1->fd->collate_key_name_nocase, di2->fd->collate_key_name_nocase);
		}
	if (mask & DUPE_MATCH_NAME_CONTENT)
		{
		return g_strcmp0(di1->fd->collate_key_name, di2->fd->collate_key_name);
		}
	if (mask & DUPE_MATCH_NAME_CI_CONTENT)
		{
		return strcmp(di1->fd->collate_key_name_nocase, di2->fd->collate_key_name_nocase);
		}
	if (mask & DUPE_MATCH_SIZE)
		{
		return (di1->fd->size - di2->fd->size);
		}
	if (mask & DUPE_MATCH_DATE)
		{
		return (di1->fd->date - di2->fd->date);
		}
	if (mask & DUPE_MATCH_SUM)
		{
		if (!di1->md5sum || di1->md5sum->empty() ||
		    !di2->md5sum || di2->md5sum->empty())
			{
			return -1;
			}

		return di1->md5sum->compare(di2->md5sum.value());
		}
	if (mask & DUPE_MATCH_DIM)
		{
		if (!di1 || !di2 || !di1->dimensions.width || !di1->dimensions.height || !di2->dimensions.width || !di2->dimensions.height)
			{
			return -1;
			}
		return (di1->dimensions_sum - di2->dimensions_sum);
		}

	return 0; // should not execute
}

/**
 * @brief Check for duplicate matches
 * @param dw
 *
 * Is not used for similarity checks.
 *
 * Loads the file sets into an array and sorts on the searched
 * for parameter.
 *
 * If one file set, steps down the array looking for adjacent equal values.
 *
 * If two file sets, steps down the first set and for each value
 * does a binary search for matches in the second set.
 */
static void dupe_array_check(DupeWindow *dw)
{
	DUPE_CHECK_RESULT check_result;
	param_match_mask = dw->match_mask;
	guint out_match_index;

	if (!dw->list) return;

	dupe_match_reset_list(dw->list);

	const auto list_to_sorted_array = [dw](GList *list)
	{
		GArray *array_set = g_array_new(TRUE, TRUE, sizeof(gpointer));

		for (GList *work = list; work; work = work->next)
			{
			g_array_append_val(array_set, work->data);
			}

		g_array_sort_with_data(array_set, dupe_match_sort_cb, dw);
		return array_set;
	};

	GArray *array_set1 = list_to_sorted_array(dw->list);

	if (dw->ignore_same_folder)
		{
		g_autoptr(GArray) array_set2 = dw->second_set ? list_to_sorted_array(dw->second_list) : nullptr;
		GArray *candidates = dw->second_set ? array_set2 : array_set1;
		for (guint i = 0; i < array_set1->len; ++i)
			{
			auto *first = static_cast<DupeItem *>(g_array_index(array_set1, gpointer, i));
			guint start = i + 1;
			if (dw->second_set)
				{
				if (!g_array_binary_search(candidates, first, dupe_match_binary_search_cb, &start)) continue;
				// Scan the entire equal-key range, even if the first candidate is in the same folder.
				while (start > 0 && dupe_match_binary_search_cb(&g_array_index(candidates, gpointer, start - 1), first) == 0) --start;
				}
			for (guint j = start; j < candidates->len; ++j)
				{
				auto *second = static_cast<DupeItem *>(g_array_index(candidates, gpointer, j));
				if (dupe_match_binary_search_cb(&g_array_index(candidates, gpointer, j), first) != 0) break;
				if (dupe_pair_allowed(dw, first, second) && dupe_match_check(first, second, dw) == DUPE_MATCH)
					{
					dupe_match_link(first, second, 0.0);
					}
				}
			}
		g_array_free(array_set1, TRUE);
		return;
		}

	if (dw->second_set)
		{
		/* Two sets - nothing can be done until a second set is loaded */
		if (dw->second_list)
			{
			GArray *array_set2 = list_to_sorted_array(dw->second_list);

			for (gint i_set1 = 0; i_set1 <= static_cast<gint>(array_set1->len) - 1; i_set1++)
				{
				auto di1 = static_cast<DupeItem *>(g_array_index(array_set1, gpointer, i_set1));
				DupeItem *di2 = nullptr;
				/* If multiple identical entries in set 1, use the last one */
				if (i_set1 < static_cast<gint>(array_set1->len) - 2)
					{
					di2 = static_cast<DupeItem *>(g_array_index(array_set1, gpointer, i_set1 + 1));
					check_result = dupe_match_check(di1, di2, dw);
					if (check_result == DUPE_MATCH || check_result == DUPE_NAME_MATCH)
						{
						continue;
						}
					}

				if (g_array_binary_search(array_set2, di1, dupe_match_binary_search_cb, &out_match_index))
					{
					di2 = static_cast<DupeItem *>(g_array_index(array_set2, gpointer, out_match_index));

					check_result = dupe_match_check(di1, di2, dw);
					if (check_result == DUPE_MATCH || check_result == DUPE_NAME_MATCH)
						{
						if (check_result == DUPE_MATCH)
							{
							dupe_match_link(di2, di1, 0.0);
							}

						gint i_set2 = out_match_index + 1;
						if (i_set2 > static_cast<gint>(array_set2->len) - 1)
							{
							break;
							}
						/* Look for multiple matches in set 2 for item di1 */
						di2 = static_cast<DupeItem *>(g_array_index(array_set2, gpointer, i_set2));
						check_result = dupe_match_check(di1, di2, dw);
						while (check_result == DUPE_MATCH || check_result == DUPE_NAME_MATCH)
							{
							if (check_result == DUPE_MATCH)
								{
								dupe_match_link(di2, di1, 0.0);
								}
							i_set2++;
							if (i_set2 > static_cast<gint>(array_set2->len) - 1)
								{
								break;
								}
							di2 = static_cast<DupeItem *>(g_array_index(array_set2, gpointer, i_set2));
							check_result = dupe_match_check(di1, di2, dw);
							}
						}
					}
				}

			g_array_free(array_set2, TRUE);
			}
		}
	else
		{
		/* File set 1 only */
		g_clear_pointer(&dw->dupes, g_list_free);

		for (gint i_set1 = 0; i_set1 <= static_cast<gint>(array_set1->len) - 2; i_set1++)
			{
			auto *di1 = static_cast<DupeItem *>(g_array_index(array_set1, gpointer, i_set1));

			/* Look for multiple matches for item di1 */
			auto *di2 = static_cast<DupeItem *>(g_array_index(array_set1, gpointer, i_set1 + 1));
			check_result = dupe_match_check(di1, di2, dw);
			while (check_result == DUPE_MATCH || check_result == DUPE_NAME_MATCH)
				{
				if (check_result == DUPE_MATCH)
					{
					dupe_match_link(di2, di1, 0.0);
					}

				i_set1++;
				if (i_set1 + 1 > static_cast<gint>(array_set1->len) - 1) break;

				di2 = static_cast<DupeItem *>(g_array_index(array_set1, gpointer, i_set1 + 1));
				check_result = dupe_match_check(di1, di2, dw);
				}
			}
		}

	g_array_free(array_set1, TRUE);
}

/**
 * @brief Look for similarity match
 * @param dw
 * @param needle
 * @param start
 *
 * Only used for similarity checks.\n
 * Called from dupe_check_cb.
 * Called for each entry in the list.
 * Steps through the list looking for matches against needle.
 * Pushes a @ref dupe_queue_item "DupeQueueItem" onto thread pool queue.
 */
static void dupe_list_check_match(DupeWindow *dw, DupeItem *needle, GList *start)
{
	GList *work;
	DupeQueueItem *dqi;

	if (dw->second_set)
		{
		work = dw->second_list;
		}
	else if (start)
		{
		work = start;
		}
	else
		{
		work = g_list_last(dw->list);
		}

	dqi = g_new0(DupeQueueItem, 1);
	dqi->needle = needle;
	dqi->dw = dw;
	dqi->work = work;
	dqi->index = dw->queue_count;
	g_thread_pool_push(dw->dupe_comparison_thread_pool, dqi, nullptr);
}

/*
 * ------------------------------------------------------------------
 * Thumbnail handling
 * ------------------------------------------------------------------
 */

static GdkPixbuf *dupe_scale_thumb(GdkPixbuf *pixbuf)
{
	if (!pixbuf) return nullptr;

	gint width;
	gint height;
	pixbuf_scale_aspect(options->thumbnails.size.width,
	                    options->thumbnails.size.height,
	                    gdk_pixbuf_get_width(pixbuf),
	                    gdk_pixbuf_get_height(pixbuf),
	                    width, height);

	if (width == gdk_pixbuf_get_width(pixbuf) && height == gdk_pixbuf_get_height(pixbuf))
		{
		return GDK_PIXBUF(g_object_ref(pixbuf));
		}

	return gdk_pixbuf_scale_simple(pixbuf, width, height, options->thumbnails.quality);
}

static void dupe_listview_set_thumb(DupeRow *row)
{
	g_clear_object(&row->thumb);
	row->thumb = dupe_scale_thumb(row->item->pixbuf);
	g_signal_emit_by_name(row, "changed");
}

static void dupe_thumb_do(DupeWindow *dw)
{
	DupeItem *di;

	if (!dw->thumb_loader || !dw->thumb_item) return;
	di = dw->thumb_item;

	if (di->pixbuf) g_object_unref(di->pixbuf);
	di->pixbuf = thumb_loader_get_pixbuf(dw->thumb_loader);

	const guint count = g_list_model_get_n_items(G_LIST_MODEL(dupe_view_store(dw->listview)));
	for (guint i = 0; i < count; ++i)
		{
		g_autoptr(DupeRow) row = dupe_view_row(dw->listview, i);
		if (row->item == di) dupe_listview_set_thumb(row);
		}
}

static void dupe_thumb_error_cb(ThumbLoader *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_thumb_do(dw);
	dupe_thumb_step(dw);
}

static void dupe_thumb_done_cb(ThumbLoader *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_thumb_do(dw);
	dupe_thumb_step(dw);
}

static void dupe_thumb_step(DupeWindow *dw)
{
	DupeItem *di = nullptr;
	const guint length = g_list_model_get_n_items(G_LIST_MODEL(dupe_view_store(dw->listview)));
	guint position = 0;
	for (; position < length; ++position)
		{
		g_autoptr(DupeRow) row = dupe_view_row(dw->listview, position);
		if (!row->thumb && row->item->pixbuf) dupe_listview_set_thumb(row);
		if (!row->thumb)
			{
			di = row->item;
			break;
			}
		}

	if (!di)
		{
		dw->thumb_item = nullptr;
		thumb_loader_free(dw->thumb_loader);
		dw->thumb_loader = nullptr;

		dupe_window_update_progress(dw, nullptr, 0.0, FALSE);
		return;
		}

	dupe_window_update_progress(dw, _("Loading thumbs…"),
				    length == 0 ? 0.0 : static_cast<gdouble>(position) / length, FALSE);

	dw->thumb_item = di;
	thumb_loader_free(dw->thumb_loader);
	dw->thumb_loader = thumb_loader_new(options->thumbnails.size.width, options->thumbnails.size.height);

	thumb_loader_set_callbacks(dw->thumb_loader,
				   dupe_thumb_done_cb,
				   dupe_thumb_error_cb,
				   nullptr,
				   dw);

	/* start it */
	if (!thumb_loader_start(dw->thumb_loader, di->fd))
		{
		/* error, handle it, do next */
		DEBUG_1("error loading thumb for %s", di->fd->path);
		dupe_thumb_do(dw);
		dupe_thumb_step(dw);
		}
}

/*
 * ------------------------------------------------------------------
 * Dupe checking loop
 * ------------------------------------------------------------------
 */

static void dupe_add_actions_set_enabled(DupeWindow *dw, gboolean enabled)
{
	for (const gchar *name : {"dupe-win-append-file-list", "dupe-win-add-folder", "dupe-win-second-append-file-list", "dupe-win-second-add-folder"})
		{
		auto *action = g_action_map_lookup_action(G_ACTION_MAP(dw->window), name);
		if (action) g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
		}
}

static void dupe_check_stop(DupeWindow *dw)
{
	if (!dw) return;

	g_clear_handle_id(&dw->recompare_id, g_source_remove);
	g_clear_handle_id(&dw->idle_id, g_source_remove);

	dw->abort = TRUE;

	while (dw->thread_count < dw->queue_count) // Wait for the queue to empty
		{
		dupe_window_update_progress(dw, nullptr, 0.0, FALSE);
		gtk_widget_set_cursor_from_name(dw->listview, nullptr);
		}

	g_list_free(dw->search_matches);
	dw->search_matches = nullptr;

	if (dw->idle_id || dw->img_loader || dw->thumb_loader)
		{
		g_clear_handle_id(&dw->idle_id, g_source_remove);
		dupe_window_update_progress(dw, nullptr, 0.0, FALSE);
		gtk_widget_set_cursor_from_name(dw->listview, nullptr);
		}

	if (dw->add_files_queue_id)
		{
		g_clear_handle_id(&dw->add_files_queue_id, g_source_remove);
		dupe_destroy_list_cache(dw);
		gtk_widget_set_sensitive(dw->controls_box, TRUE);
		dupe_add_actions_set_enabled(dw, TRUE);
		if (g_list_length(dw->add_files_queue) > 0)
			{
			file_data_list_free(dw->add_files_queue);
			}
		dw->add_files_queue = nullptr;
		dupe_window_update_progress(dw, nullptr, 0.0, FALSE);
		gtk_widget_set_cursor_from_name(dw->listview, nullptr);
		}

	thumb_loader_free(dw->thumb_loader);
	dw->thumb_loader = nullptr;

	g_clear_object(&dw->img_loader);
}

static void dupe_check_stop_cb(GtkWidget *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_check_stop(dw);
}

static void dupe_loader_done_cb(ImageLoader *il, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);
	GdkPixbuf *pixbuf;

	pixbuf = image_loader_get_pixbuf(il);

	if (dw->setup_point)
		{
		auto di = static_cast<DupeItem *>(dw->setup_point->data);

		if (!di->simd)
			{
			di->simd = std::make_unique<ImageSimilarityData>();
			}

		di->simd->fill_data(pixbuf);

		if (di->dimensions.empty() && pixbuf)
			{
			di->dimensions.width = gdk_pixbuf_get_width(pixbuf);
			di->dimensions.height = gdk_pixbuf_get_height(pixbuf);
			}
		if (options->thumbnails.enable_caching)
			{
			dupe_item_write_cache(di);
			}

		}

	g_clear_object(&dw->img_loader);

	dw->idle_id = g_idle_add(dupe_check_cb, dw);
}

static void dupe_setup_reset(DupeWindow *dw)
{
	dw->setup_point = nullptr;
	dw->setup_n = 0;
	dw->setup_time = msec_time();
	dw->setup_time_count = 0;
}

static GList *dupe_setup_point_step(DupeWindow *dw, GList *p)
{
	if (!p) return nullptr;

	if (p->next) return p->next;

	if (dw->second_set && g_list_first(p) == dw->list) return dw->second_list;

	return nullptr;
}

/**
 * @brief Generates the sumcheck or dimensions_sum
 * @param dw Duplicate comparison window
 * @param list Set1 or set2
 * @returns TRUE/FALSE = not completed/completed
 *
 * Ensures that the DIs contain the MD5SUM or dimensions_sum for all items in
 * the list. One item at a time. Re-enters if not completed.
 */
static gboolean create_checksums_dimensions(DupeWindow *dw, GList *list)
{
	static const auto setup_progress = [](const DupeWindow *dw)
	{
		return (dw->setup_count == 0) ? 0.0 : static_cast<gdouble>(dw->setup_n - 1) / dw->setup_count;
	};

	if ((dw->match_mask & DUPE_MATCH_SUM) ||
	    (dw->match_mask & DUPE_MATCH_NAME_CONTENT) ||
	    (dw->match_mask & DUPE_MATCH_NAME_CI_CONTENT))
		{
		/* MD5SUM only */
		if (!dw->setup_point) dw->setup_point = list; // setup_point clear on 1st entry

		while (dw->setup_point)
			{
			auto di = static_cast<DupeItem *>(dw->setup_point->data);

			dw->setup_point = dupe_setup_point_step(dw, dw->setup_point);
			dw->setup_n++;

			if (!di->md5sum)
				{
				dupe_window_update_progress(dw, _("Reading checksums…"), setup_progress(dw), FALSE);

				if (options->thumbnails.enable_caching)
					{
					dupe_item_read_cache(di);
					if (di->md5sum) return TRUE;
					}

				di->md5sum = md5_text_from_file_utf8(di->fd->path);
				if (options->thumbnails.enable_caching)
					{
					dupe_item_write_cache(di);
					}
				return TRUE;
				}
			}
		dupe_setup_reset(dw);
		}

	if (dw->match_mask & DUPE_MATCH_DIM)
		{
		/* Dimensions only */
		if (!dw->setup_point) dw->setup_point = list;

		while (dw->setup_point)
			{
			auto di = static_cast<DupeItem *>(dw->setup_point->data);

			dw->setup_point = dupe_setup_point_step(dw, dw->setup_point);
			dw->setup_n++;
			if (di->dimensions.empty())
				{
				dupe_window_update_progress(dw, _("Reading dimensions…"), setup_progress(dw), FALSE);

				if (options->thumbnails.enable_caching)
					{
					dupe_item_read_cache(di);
					if (!di->dimensions.empty()) return TRUE;
					}

				image_load_dimensions(di->fd, di->dimensions);
				di->dimensions_sum = (di->dimensions.width << 16) + di->dimensions.height;
				if (options->thumbnails.enable_caching)
					{
					dupe_item_write_cache(di);
					}
				return TRUE;
				}
			}
		dupe_setup_reset(dw);
		}

	return FALSE;
}

/**
 * @brief Compare func. for sorting search matches
 * @param a @ref dupe_search_match "DupeSearchMatch"
 * @param b @ref dupe_search_match "DupeSearchMatch"
 * @returns
 *
 * Used only for similarity checks\n
 * Sorts search matches on order they were inserted into the pool queue
 */
static gint sort_func(gconstpointer a, gconstpointer b)
{
	return static_cast<const DupeSearchMatch *>(a)->index - static_cast<const DupeSearchMatch *>(b)->index;
}

/**
 * @brief Check set 1 (and set 2) for matches
 * @param data DupeWindow
 * @returns TRUE/FALSE = not completed/completed
 *
 * Initiated from start, loader done and item remove
 *
 * On first entry generates di->MD5SUM, di->dimensions and sim data,
 * and updates the cache.
 */
static gboolean dupe_check_cb(gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);
	DupeSearchMatch *search_match_list_item;

	if (!dw->idle_id)
		{
		return G_SOURCE_REMOVE;
		}

	if (!dw->setup_done) /* Clear on 1st entry */
		{
		if (dw->list)
			{
			if (create_checksums_dimensions(dw, dw->list))
				{
				return G_SOURCE_CONTINUE;
				}
			}
		if (dw->second_list)
			{
			if (create_checksums_dimensions(dw, dw->second_list))
				{
				return G_SOURCE_CONTINUE;
				}
			}
		if ((dw->match_mask & DUPE_MATCH_SIM) &&
		    !(dw->setup_mask & DUPE_MATCH_SIM_MED) )
			{
			/* Similarity only */
			if (!dw->setup_point) dw->setup_point = dw->list;

			while (dw->setup_point)
				{
				auto di = static_cast<DupeItem *>(dw->setup_point->data);

				if (!di->simd)
					{
					dupe_window_update_progress(dw, _("Reading similarity data…"),
						dw->setup_count == 0 ? 0.0 : static_cast<gdouble>(dw->setup_n) / dw->setup_count, FALSE);

					if (options->thumbnails.enable_caching)
						{
						dupe_item_read_cache(di);
						if (image_sim_filled(di->simd.get()))
							{
							return G_SOURCE_CONTINUE;
							}
						}

					dw->img_loader = image_loader_new(di->fd);
					image_loader_set_buffer_size(dw->img_loader, 8);
					g_signal_connect(G_OBJECT(dw->img_loader), "error", (GCallback)dupe_loader_done_cb, dw);
					g_signal_connect(G_OBJECT(dw->img_loader), "done", (GCallback)dupe_loader_done_cb, dw);

					if (!image_loader_start(dw->img_loader))
						{
						di->simd = std::make_unique<ImageSimilarityData>();
						g_clear_object(&dw->img_loader);
						return G_SOURCE_CONTINUE;
						}
					dw->idle_id = 0;
					return G_SOURCE_REMOVE;
					}

				if (options->alternate_similarity_algorithm.enabled && di->simd &&
				    di->simd->ensure_perceptual_hashes() && options->thumbnails.enable_caching)
					{
					dupe_item_write_cache(di);
					}

				dw->setup_point = dupe_setup_point_step(dw, dw->setup_point);
				dw->setup_n++;
				}
			dw->setup_mask = static_cast<DupeMatchType>(dw->setup_mask | DUPE_MATCH_SIM_MED);
			dupe_setup_reset(dw);
			}

		/* End of setup not done */
		dupe_window_update_progress(dw, _("Comparing…"), 0.0, FALSE);
		dw->setup_done = TRUE;
		dupe_setup_reset(dw);
		dw->setup_count = g_list_length(dw->list);
		}

	/* Setup done - dw->working set to NULL below
	 * Set before 1st entry: dw->working = g_list_last(dw->list)
	 * Set before 1st entry: dw->setup_count = g_list_length(dw->list)
	 */
	if (!dw->working)
		{
		/* Similarity check threads may still be running */
		if (dw->setup_count > 0 && (dw->match_mask & DUPE_MATCH_SIM))
			{
			if( dw->thread_count < dw->queue_count)
				{
				g_autofree gchar *progress_text = g_strdup_printf("%s %d/%d", _("Comparing"), dw->thread_count, dw->queue_count);

				dupe_window_update_progress(dw, progress_text, (gdouble)dw->thread_count / dw->queue_count, TRUE);

				return G_SOURCE_CONTINUE;
				}

			if (dw->search_matches_sorted == nullptr)
				{
				dw->search_matches_sorted = g_list_sort(dw->search_matches, sort_func);
				dupe_setup_reset(dw);
				}

			while (dw->search_matches_sorted)
				{
				dw->setup_n++;
				dupe_window_update_progress(dw, _("Sorting…"), 0.0, FALSE);
				search_match_list_item = static_cast<DupeSearchMatch *>(dw->search_matches_sorted->data);

				if (!dupe_match_link_exists(search_match_list_item->a, search_match_list_item->b))
					{
					dupe_match_link(search_match_list_item->a, search_match_list_item->b, search_match_list_item->rank);
					}

				dw->search_matches_sorted = dw->search_matches_sorted->next;

				if (dw->search_matches_sorted != nullptr)
					{
					return G_SOURCE_CONTINUE;
					}
				}
			g_list_free(dw->search_matches); // FIXME dw->search_matches doesn't point at the start of the list after g_list_sort() call
			dw->search_matches = nullptr;
			dw->search_matches_sorted = nullptr;
			dw->setup_count = 0;
			}
		else
			{
			if (dw->setup_count > 0)
				{
				dw->setup_count = 0;
				dupe_window_update_progress(dw, _("Sorting…"), 1.0, TRUE);
				return G_SOURCE_CONTINUE;
				}
			}

		dw->idle_id = 0;
		dupe_window_update_progress(dw, nullptr, 0.0, FALSE);

		dupe_match_rank(dw);
		dupe_window_update_count(dw, FALSE);

		dupe_listview_populate(dw);

		/* check thumbs */
		if (dw->show_thumbs) dupe_thumb_step(dw);

		gtk_widget_set_cursor_from_name(dw->listview, nullptr);

		return G_SOURCE_REMOVE;
		/* The end */
		}

	/* Setup done - working */
	if (dw->match_mask & DUPE_MATCH_SIM)
		{
		/* This is the similarity comparison */
		dupe_list_check_match(dw, static_cast<DupeItem *>(dw->working->data), dw->working);
		dupe_window_update_progress(dw, _("Queuing…"), dw->setup_count == 0 ? 0.0 : static_cast<gdouble>(dw->setup_n) / dw->setup_count, FALSE);
		dw->setup_n++;
		dw->queue_count++;

		dw->working = dw->working->prev; /* Is NULL when complete */
		}
	else
		{
		/* This is the comparison for all other parameters.
		 * dupe_array_check() processes the entire list in one go
		*/
		dw->working = nullptr;
		dupe_window_update_progress(dw, _("Comparing…"), 0.0, FALSE);
		dupe_array_check(dw);
		}

	return G_SOURCE_CONTINUE;
}

static void dupe_check_start(DupeWindow *dw)
{
	g_clear_handle_id(&dw->recompare_id, g_source_remove);
	dw->comparison_perceptual = options->alternate_similarity_algorithm.enabled;
	dw->comparison_rotation_invariant = options->rot_invariant_sim;
	dw->comparison_threshold = dw->match_mask & DUPE_MATCH_SIM_HIGH ? 95 :
	                           dw->match_mask & DUPE_MATCH_SIM_MED ? 90 :
	                           dw->match_mask & DUPE_MATCH_SIM_CUSTOM ? options->duplicates_similarity_threshold : 85;
	dw->setup_done = FALSE;

	dw->setup_count = g_list_length(dw->list);
	if (dw->second_set) dw->setup_count += g_list_length(dw->second_list);

	dw->setup_mask = DUPE_MATCH_NONE;
	dupe_setup_reset(dw);

	dw->working = g_list_last(dw->list);

	dupe_window_update_count(dw, TRUE);
	gtk_widget_set_cursor_from_name(dw->listview, "wait");
	dw->queue_count = 0;
	dw->thread_count = 0;
	dw->search_matches_sorted = nullptr;
	dw->abort = FALSE;

	if (dw->idle_id) return;

	dw->idle_id = g_idle_add(dupe_check_cb, dw);
}

static gboolean dupe_check_start_cb(gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	dw->recompare_id = 0;
	dupe_check_start(dw);

	return G_SOURCE_REMOVE;
}

/*
 * ------------------------------------------------------------------
 * Item addition, removal
 * ------------------------------------------------------------------
 */

static void dupe_item_remove(DupeWindow *dw, DupeItem *di)
{
	if (!di) return;

	/* handle things that may be in progress… */
	if (dw->working && dw->working->data == di)
		{
		dw->working = dw->working->prev;
		}
	if (dw->thumb_loader && dw->thumb_item == di)
		{
		dupe_thumb_step(dw);
		}
	if (dw->setup_point && dw->setup_point->data == di)
		{
		dw->setup_point = dupe_setup_point_step(dw, dw->setup_point);
		if (dw->img_loader)
			{
			g_clear_object(&dw->img_loader);
			dw->idle_id = g_idle_add(dupe_check_cb, dw);
			}
		}

	if (dw->ignore_same_folder)
		{
		dupe_check_stop(dw);
		dupe_match_link_clear(di, TRUE);
		g_clear_pointer(&dw->dupes, g_list_free);
		g_list_store_remove_all(dupe_view_store(dw->listview));
		}
	else if (di->group && dw->dupes)
		{
		/* is a dupe, must remove from group/reset children if a parent */
		DupeItem *parent;

		parent = dupe_match_find_parent(dw, di);
		if (di == parent)
			{
			if (g_list_length(parent->group) < 2)
				{
				DupeItem *child;

				child = dupe_match_highest_rank(parent);
				dupe_match_link_clear(child, TRUE);
				dupe_listview_remove(dw, child);

				dupe_match_link_clear(parent, TRUE);
				dupe_listview_remove(dw, parent);
				dw->dupes = g_list_remove(dw->dupes, parent);
				}
			else
				{
				DupeItem *new_parent;
				DupeMatch *dm;

				dm = static_cast<DupeMatch *>(parent->group->data);
				new_parent = dm->di;
				dupe_match_reparent(dw, parent, new_parent);
				dupe_listview_remove(dw, parent);
				}
			}
		else
			{
			if (g_list_length(parent->group) < 2)
				{
				dupe_match_link_clear(parent, TRUE);
				dupe_listview_remove(dw, parent);
				dw->dupes = g_list_remove(dw->dupes, parent);
				}
			dupe_match_link_clear(di, TRUE);
			dupe_listview_remove(dw, di);
			}
		}
	else
		{
		/* not a dupe, or not sorted yet, simply reset */
		dupe_match_link_clear(di, TRUE);
		}

	if (dw->second_list && g_list_find(dw->second_list, di))
		{
		dupe_second_remove(dw, di);
		}
	else
		{
		dw->list = g_list_remove(dw->list, di);
		}
	dupe_item_free(di);

	if (dw->ignore_same_folder) dupe_window_recompare_idle(dw);
	dupe_window_update_count(dw, FALSE);
}

static gboolean dupe_files_add_queue_cb(gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);

	gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dw->extra_label));

	if (dw->add_files_queue != nullptr)
		{
		DupeItem *di = nullptr;

		auto *fd = static_cast<FileData *>(dw->add_files_queue->data);
		if (fd)
			{
			if (isfile(fd->path))
				{
				di = dupe_item_new(fd);
				}
			else if (isdir(fd->path))
				{
				GList *f;
				GList *d;
				dw->add_files_queue = g_list_remove(dw->add_files_queue, g_list_first(dw->add_files_queue)->data);

				if (filelist_read(fd, &f, &d))
					{
					f = filelist_filter(f, FALSE);
					d = filelist_filter(d, TRUE);

					dw->add_files_queue = g_list_concat(f, dw->add_files_queue);
					dw->add_files_queue = g_list_concat(d, dw->add_files_queue);
					}
				}
			else
				{
				/* Not a file and not a dir */
				dw->add_files_queue = g_list_remove(dw->add_files_queue, g_list_first(dw->add_files_queue)->data);
				}
			}

		if (!di)
			{
			/* A dir was found. Process the contents on next entry */
			return G_SOURCE_CONTINUE;
			}

		dw->add_files_queue = g_list_remove(dw->add_files_queue, g_list_first(dw->add_files_queue)->data);

		dupe_item_read_cache(di);

		/* Ensure images in the lists have unique FileDatas */
		if (!dupe_insert_in_list_cache(dw, di->fd))
			{
			dupe_item_free(di);
			return G_SOURCE_CONTINUE;
			}

		if (dw->second_drop)
			{
			dupe_second_add(dw, di);
			}
		else
			{
			dw->list = g_list_prepend(dw->list, di);
			}

		if (dw->add_files_queue != nullptr)
			{
			return G_SOURCE_CONTINUE;
			}
		}

	dw->add_files_queue_id = 0;
	dupe_destroy_list_cache(dw);
	g_clear_handle_id(&dw->recompare_id, g_source_remove);
	dw->recompare_id = g_idle_add(dupe_check_start_cb, dw);
	gtk_widget_set_sensitive(dw->controls_box, TRUE);
	dupe_add_actions_set_enabled(dw, TRUE);
	return G_SOURCE_REMOVE;
}

static void dupe_files_add(DupeWindow *dw, CollectInfo *info,
                           FileData *fd, gboolean recurse)
{
	DupeItem *di = nullptr;

	if (info)
		{
		di = dupe_item_new(info->fd);
		}
	else if (fd)
		{
		if (isfile(fd->path) && !g_file_test(fd->path, G_FILE_TEST_IS_SYMLINK))
			{
			di = dupe_item_new(fd);
			}
		else if (isdir(fd->path) && recurse)
			{
			GList *f;
			GList *d;
			if (filelist_read(fd, &f, &d))
				{
				GList *work;

				f = filelist_filter(f, FALSE);
				d = filelist_filter(d, TRUE);

				work = f;
				while (work)
					{
					dupe_files_add(dw, nullptr, static_cast<FileData *>(work->data), TRUE);
					work = work->next;
					}
				file_data_list_free(f);
				work = d;
				while (work)
					{
					dupe_files_add(dw, nullptr, static_cast<FileData *>(work->data), TRUE);
					work = work->next;
					}
				file_data_list_free(d);
				}
			}
		}

	if (!di) return;

	dupe_item_read_cache(di);

	/* Ensure images in the lists have unique FileDatas */
	GList *work;
	DupeItem *di_list;
	work = g_list_first(dw->list);
	while (work)
		{
		di_list = static_cast<DupeItem *>(work->data);
		if (di_list->fd == di->fd)
			{
			return;
			}

		work = work->next;
		}

	if (dw->second_list)
		{
		work = g_list_first(dw->second_list);
		while (work)
			{
			di_list = static_cast<DupeItem *>(work->data);
			if (di_list->fd == di->fd)
				{
				return;
				}

			work = work->next;
			}
		}

	if (dw->second_drop)
		{
		dupe_second_add(dw, di);
		}
	else
		{
		dw->list = g_list_prepend(dw->list, di);
		}
}

static void dupe_init_list_cache(DupeWindow *dw)
{
	dw->list_cache = g_hash_table_new(g_direct_hash, g_direct_equal);
	dw->second_list_cache = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (GList *i = dw->list; i != nullptr; i = i->next)
		{
			auto di = static_cast<DupeItem *>(i->data);

			g_hash_table_add(dw->list_cache, di->fd);
		}

	for (GList *i = dw->second_list; i != nullptr; i = i->next)
		{
			auto di = static_cast<DupeItem *>(i->data);

			g_hash_table_add(dw->second_list_cache, di->fd);
		}
}

static void dupe_destroy_list_cache(DupeWindow *dw)
{
	g_hash_table_destroy(dw->list_cache);
	g_hash_table_destroy(dw->second_list_cache);
}

/**
 * @brief Return true if the fd was not in the cache
 * @param dw
 * @param fd
 * @returns
 *
 *
 */
static gboolean dupe_insert_in_list_cache(DupeWindow *dw, FileData *fd)
{
	GHashTable *table =
		dw->second_drop ? dw->second_list_cache : dw->list_cache;
	/* We do this as a lookup + add as we don't want to overwrite
	   items as that would leak the old value. */
	if (g_hash_table_contains(table, fd))
		return FALSE;
	return g_hash_table_add(table, fd);
}

void dupe_window_add_collection(DupeWindow *dw, CollectionData *collection)
{
	CollectInfo *info;

	info = collection_get_first(collection);
	while (info)
		{
		dupe_files_add(dw, info, nullptr, FALSE);
		info = collection_next_by_info(collection, info);
		}

	dupe_check_start(dw);
}

void dupe_window_add_files(DupeWindow *dw, GList *list, gboolean recurse)
{
	GList *work;

	work = list;
	while (work)
		{
		auto fd = static_cast<FileData *>(work->data);
		work = work->next;
		if (isdir(fd->path) && !recurse)
			{
			GList *f;
			GList *d;

			if (filelist_read(fd, &f, &d))
				{
				GList *work_file;
				work_file = f;

				while (work_file)
					{
					/* Add only the files, ignore the dirs when no recurse */
					dw->add_files_queue = g_list_prepend(dw->add_files_queue, work_file->data);
					file_data_ref((FileData *)work_file->data);
					work_file = work_file->next;
					}
				g_list_free(f);
				g_list_free(d);
				}
			}
		else
			{
			dw->add_files_queue = g_list_prepend(dw->add_files_queue, fd);
			file_data_ref(fd);
			}
		}
	if (dw->add_files_queue_id == 0)
		{
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dw->extra_label));
		gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(dw->extra_label), DUPE_PROGRESS_PULSE_STEP);
		gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dw->extra_label), _("Loading file list"));

		dupe_init_list_cache(dw);
		dw->add_files_queue_id = g_idle_add(dupe_files_add_queue_cb, dw);
		gtk_widget_set_sensitive(dw->controls_box, FALSE);
		dupe_add_actions_set_enabled(dw, FALSE);
		}
}

void dupe_window_add_folder(const gchar *path, gboolean recurse)
{
	DupeWindow *dw = dupe_window_new();
	FileData *fd = file_data_new_simple(path);
	g_autoptr(GList) list = nullptr;
	list = g_list_append(list, fd);

	dupe_window_add_files(dw, list, recurse);

	dupe_check_start(dw);

	file_data_unref(fd);
}

static void dupe_item_update(DupeWindow *dw, DupeItem *di)
{
	if (dw->ignore_same_folder)
		{
		dupe_window_recompare_idle(dw);
		return;
		}
	if ( (dw->match_mask & DUPE_MATCH_NAME) || (dw->match_mask & DUPE_MATCH_PATH || (dw->match_mask & DUPE_MATCH_NAME_CI)) )
		{
		/* only effects matches on name or path */
/*
		FileData *fd = file_data_ref(di->fd);
		gint second;

		second = di->second;
		dupe_item_remove(dw, di);

		dw->second_drop = second;
		dupe_files_add(dw, nullptr, fd, FALSE);
		dw->second_drop = FALSE;

		file_data_unref(fd);
*/
		dupe_check_start(dw);
		}
	else
		{
		for (auto *view : {dw->listview, dw->second_listview})
			{
			const guint count = g_list_model_get_n_items(G_LIST_MODEL(dupe_view_store(view)));
			for (guint i = 0; i < count; ++i)
				{
				g_autoptr(DupeRow) row = dupe_view_row(view, i);
				if (row->item == di) g_signal_emit_by_name(row, "changed");
				}
			dupe_view_sort(view);
			}
		}

}

static void dupe_item_update_fd_in_list(DupeWindow *dw, FileData *fd, GList *work)
{
	while (work)
		{
		auto di = static_cast<DupeItem *>(work->data);

		if (di->fd == fd)
			dupe_item_update(dw, di);

		work = work->next;
		}
}

static void dupe_item_update_fd(DupeWindow *dw, FileData *fd)
{
	dupe_item_update_fd_in_list(dw, fd, dw->list);
	if (dw->second_set) dupe_item_update_fd_in_list(dw, fd, dw->second_list);
}


/*
 * ------------------------------------------------------------------
 * Misc.
 * ------------------------------------------------------------------
 */

static GtkWidget *dupe_display_label(GtkWidget *vbox, const gchar *description, const gchar *text)
{
	GtkWidget *hbox;
	GtkWidget *label;

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

	label = gtk_label_new(description);
	gtk_box_append(GTK_BOX(hbox), label);

	label = gtk_label_new(text);
	gtk_box_append(GTK_BOX(hbox), label);

	gtk_box_append(GTK_BOX(vbox), hbox);

	return label;
}

static void dupe_display_stats(DupeWindow *dw, DupeItem *di)
{
	GenericDialog *gd;

	if (!di) return;

	gd = file_util_gen_dlg(_("Image thumbprint debug info"), "thumbprint",
			       dw->window, TRUE,
			       nullptr, nullptr);
	generic_dialog_add_button(gd, GQ_ICON_CLOSE, _("Close"), nullptr, TRUE);

	dupe_display_label(gd->vbox, "name:", di->fd->name);

	g_autofree gchar *size_buf = text_from_size(di->fd->size);
	dupe_display_label(gd->vbox, "size:", size_buf);

	dupe_display_label(gd->vbox, "date:", text_from_time(di->fd->date));

	g_autofree gchar *dimensions_buf = g_strdup_printf("%d x %d", di->dimensions.width, di->dimensions.height);
	dupe_display_label(gd->vbox, "dimensions:", dimensions_buf);

	dupe_display_label(gd->vbox, "md5sum:", di->md5sum.value_or("not generated"s).c_str());

	dupe_display_label(gd->vbox, "thumbprint:", (di->simd) ? "" : "not generated");
	if (di->simd)
		{
		g_autoptr(GdkPixbuf) pixbuf = di->simd->to_pixbuf();

		g_autoptr(GdkTexture) texture = pixbuf_to_texture(pixbuf);
		GtkWidget *image = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
		gtk_box_append(GTK_BOX(gd->vbox), image);
		}

	gtk_window_present(GTK_WINDOW(gd->dialog));
}

static void dupe_window_recompare(DupeWindow *dw)
{
	dupe_check_stop(dw);
	g_list_store_remove_all(dupe_view_store(dw->listview));

	g_list_free(dw->dupes);
	dw->dupes = nullptr;

	dupe_match_reset_list(dw->list);
	dupe_match_reset_list(dw->second_list);
	dw->set_count = 0;

	dupe_check_start(dw);
}

static void dupe_window_recompare_idle(DupeWindow *dw)
{
	dupe_check_stop(dw);
	dw->recompare_id = g_idle_add([](gpointer data) -> gboolean
		{
		auto *window = static_cast<DupeWindow *>(data);
		window->recompare_id = 0;
		dupe_window_recompare(window);
		return G_SOURCE_REMOVE;
		}, dw);
}

static void dupe_menu_view(DupeItem *di, GtkWidget *listview, gboolean new_window)
{
	if (!di) return;

	if (di->collection && collection_info_valid(di->collection, di->info))
		{
		if (new_window)
			{
			view_window_new_from_collection(di->collection, di->info);
			}
		else
			{
			layout_image_set_collection(nullptr, di->collection, di->info);
			}
		}
	else
		{
		if (new_window)
			{
			g_autoptr(FileDataList) list = dupe_listview_get_selection(listview);
			view_window_new_from_list(list);
			}
		else
			{
			layout_set_fd(nullptr, di->fd);
			}
		}
}

static void dupe_window_remove_selection(DupeWindow *dw, GtkWidget *listview)
{
	GList *list = nullptr;
	auto *selection = dupe_view_selection(listview);
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(selection));
	for (guint i = 0; i < count; ++i)
		{
		if (!gtk_selection_model_is_selected(selection, i)) continue;
		auto *item = dupe_listview_item_at(listview, i);
		if (!g_list_find(list, item)) list = g_list_prepend(list, item);
		}

	dw->color_frozen = TRUE;
	GList *work = list;
	while (work)
		{
		DupeItem *di;

		di = static_cast<DupeItem *>(work->data);
		work = work->next;
		dupe_item_remove(dw, di);
		}
	dw->color_frozen = FALSE;

	g_list_free(list);

	dupe_listview_realign_colors(dw);
}

static void dupe_window_delete_selected(DupeWindow *dw, gboolean safe_delete)
{
	// Successful file-change notifications update every duplicates window, even
	// after a partial failure or a selection change while the operation runs.
	file_util_delete(nullptr, dupe_listview_get_selection(dw->listview), dw->window, safe_delete);
}

static void dupe_window_edit_selected(DupeWindow *dw, const gchar *key)
{
	file_util_start_editor_from_filelist(key, dupe_listview_get_selection(dw->listview), nullptr, dw->window);
}

static void dupe_window_collection_from_selection(DupeWindow *dw)
{
	g_autoptr(FileDataList) list = dupe_listview_get_selection(dw->listview);
	collection_by_path_add_filelist(nullptr, list);
}

static void dupe_window_append_file_list(DupeWindow *dw, gint on_second)
{
	dw->second_drop = (dw->second_set && on_second);

	g_autoptr(FileDataList) list = layout_list(nullptr);
	dupe_window_add_files(dw, list, FALSE);
}

/*
 *-------------------------------------------------------------------
 * main pop-up menu callbacks
 *-------------------------------------------------------------------
 */

template<gboolean new_window>
static void dupe_menu_view_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dw->click_item) dupe_menu_view(dw->click_item, dw->listview, new_window);
}

static void dupe_menu_select_all_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	options->duplicates_select_type = DUPE_SELECT_NONE;
	gtk_selection_model_select_all(dupe_view_selection(dw->listview));
}

static void dupe_menu_select_none_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	options->duplicates_select_type = DUPE_SELECT_NONE;
	gtk_selection_model_unselect_all(dupe_view_selection(dw->listview));
}

template<gboolean selected>
static void dupe_menu_select_all_focused_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	auto *selection = dupe_view_selection(dupe_view_has_focus(dw->second_listview) ? dw->second_listview : dw->listview);
	options->duplicates_select_type = DUPE_SELECT_NONE;
	if (selected) gtk_selection_model_select_all(selection);
	else gtk_selection_model_unselect_all(selection);
}

template<DupeSelectType parents>
static void dupe_menu_select_dupes_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	options->duplicates_select_type = parents;
	dupe_listview_select_dupes(dw, parents);
}

static void dupe_menu_edit_cb(GSimpleAction *, GVariant *parameter, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	if (!dw) return;

	const char *key = g_variant_get_string(parameter, nullptr);

	dupe_window_edit_selected(dw, key);
}

static void dupe_menu_print_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	print_window_new(dupe_listview_get_selection(dw->listview), dw->window);
}

static void dupe_menu_copy_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dupe_view_has_focus(dw->second_listview)) return;

	file_util_copy(nullptr, dupe_listview_get_selection(dw->listview), nullptr, dw->window);
}

static void dupe_menu_move_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dupe_view_has_focus(dw->second_listview)) return;

	file_util_move(nullptr, dupe_listview_get_selection(dw->listview), nullptr, dw->window);
}

static void dupe_menu_rename_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dupe_view_has_focus(dw->second_listview)) return;

	file_util_rename(nullptr, dupe_listview_get_selection(dw->listview), dw->window);
}

template<gboolean safe_delete>
static void dupe_menu_delete_cb(GSimpleAction *, GVariant *, gpointer data)
{
	dupe_window_delete_selected(static_cast<DupeWindow *>(data), safe_delete);
}

static void dupe_menu_delete_focused_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dupe_view_has_focus(dw->second_listview)) return;

	dupe_window_delete_selected(dw, TRUE);
}

template<gboolean quoted>
static void dupe_menu_copy_path_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	file_util_path_list_to_clipboard(dupe_listview_get_selection(dw->listview), quoted, ClipboardAction::COPY);
}

static void dupe_menu_remove_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_window_remove_selection(dw, dw->listview);
}

static void dupe_menu_remove_focused_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);
	GtkWidget *listview = dupe_view_has_focus(dw->second_listview) ? dw->second_listview : dw->listview;

	dupe_window_remove_selection(dw, listview);
}

static void dupe_menu_clear_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_window_clear(dw);
}

static void dupe_menu_clear_focused_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dupe_view_has_focus(dw->second_listview))
		{
		dupe_second_clear(dw);
		dupe_window_recompare(dw);
		}
	else
		{
		dupe_window_clear(dw);
		}
}

static DupeItem *dupe_listview_get_last_selected_item(GtkWidget *view)
{
	g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(dupe_view_selection(view));
	return gtk_bitset_is_empty(selected) ? nullptr : dupe_listview_item_at(view, gtk_bitset_get_maximum(selected));
}

template<gboolean new_window>
static void dupe_menu_view_focused_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);
	GtkWidget *listview = dupe_view_has_focus(dw->second_listview) ? dw->second_listview : dw->listview;

	dupe_menu_view(dupe_listview_get_last_selected_item(listview), listview, new_window);
}

static void dupe_menu_collection_from_selection_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dupe_view_has_focus(dw->second_listview)) return;

	dupe_window_collection_from_selection(dw);
}

template<bool second>
static void dupe_menu_append_file_list_cb(GSimpleAction *, GVariant *, gpointer data)
{
	dupe_window_append_file_list(static_cast<DupeWindow *>(data), second);
}

struct DupeFolderDialog
{
	GtkWidget *window;
	bool second;
	gboolean recursive = FALSE;

	~DupeFolderDialog() { g_object_unref(window); }
};

static void dupe_add_folder_selected_cb(GFile *file, gpointer data)
{
	std::unique_ptr<DupeFolderDialog> dialog(static_cast<DupeFolderDialog *>(data));
	auto *dw = static_cast<DupeWindow *>(g_object_get_data(G_OBJECT(dialog->window), DUPE_WINDOW_DATA_KEY));
	if (!file || !dw) return;
	g_autofree gchar *path = g_file_get_path(file);
	if (!path) return;
	g_autoptr(FileDataList) list = g_list_append(nullptr, file_data_new_dir(path));
	dw->second_drop = dw->second_set && dialog->second;
	dupe_window_add_files(dw, list, dialog->recursive);
}

template<bool second>
static void dupe_menu_add_folder_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	auto *dialog = new DupeFolderDialog{GTK_WIDGET(g_object_ref(dw->window)), second};
	FileDialogData fdd{};
	fdd.action = FileDialogAction::SELECT_FOLDER;
	fdd.title = _("Add folder to duplicate search");
	fdd.accept_text = _("Add");
	fdd.history_key = "dupe_add_folder";
	fdd.checkbox_text = _("Include subfolders");
	fdd.checkbox_value = &dialog->recursive;
	fdd.callback = dupe_add_folder_selected_cb;
	fdd.data = dialog;
	fdd.parent = GTK_WINDOW(dw->window);
	file_dialog_show(fdd);
}

static void dupe_menu_popup_focused_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dupe_view_has_focus(dw->second_listview))
		{
		dupe_menu_popup_second(dw);
		}
	else
		{
		dupe_menu_popup_main(dw);
		}
}

static void dupe_menu_toggle_thumbnails_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	auto *button_thumbs = GTK_CHECK_BUTTON(dw->button_thumbs);
	gtk_check_button_set_active(button_thumbs, !gtk_check_button_get_active(button_thumbs));
}

static void dupe_menu_close_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_window_close(dw);
}

static GList *dupe_window_get_fd_list(DupeWindow *dw)
{
	GList *list;

	if (dupe_view_has_focus(dw->second_listview))
		{
		list = dupe_listview_get_selection(dw->second_listview);
		}
	else
		{
		list = dupe_listview_get_selection(dw->listview);
		}

	return list;
}

/**
 * @brief Add file selection list to a collection
 * @param[in] parameter Collection path, or an empty string for a new collection
 * @param[in] data Duplicate comparison window
 *
 *
 */
static void dupe_pop_menu_collections_cb(GSimpleAction *, GVariant *parameter, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	const gchar *path = g_variant_get_string(parameter, nullptr);

	g_autoptr(FileDataList) selection_list = dupe_listview_get_selection(dw->listview);
	collection_by_path_add_filelist(path, selection_list);
}

static void dupe_popup_menu_centered(GMenu *menu_model, DupeWindow *dw, GtkWidget *pane)
{
	graphene_rect_t bounds;
	if (gtk_widget_compute_bounds(pane, dw->window, &bounds))
		{
		popup_menu_at(menu_model, dw->window,
		              bounds.origin.x + (bounds.size.width / 2.0),
		              bounds.origin.y + (bounds.size.height / 2.0));
		return;
		}

	popup_menu(menu_model, dw->window);
}

static GtkWidget *dupe_menu_popup_main(DupeWindow *dw)
{
	g_autoptr(GtkBuilder) builder = gtk_builder_new_from_resource(GQ_RESOURCE_PATH_UI "/menu-dupe-main.ui");
	GMenu *menu_model = G_MENU(gtk_builder_get_object(builder, "menu-dupe-main"));

	GList *editmenu_fd_list = dupe_window_get_fd_list(dw);

	GMenu *plugins_menu = G_MENU(gtk_builder_get_object(builder, "plugins-submenu"));
	plugins_menu_populate(plugins_menu, "win.dupe-win-plugin-run", editmenu_fd_list);

	GMenu *collections_menu = G_MENU(gtk_builder_get_object(builder, "collections-submenu"));
	submenu_add_collections_new(collections_menu, "win.dupe-win-collections");

	dupe_popup_menu_centered(menu_model, dw, dw->listview);

	return nullptr;
}

static void dupe_listview_press_cb(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data)
{
	GtkWidget *event_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	GtkWidget *view = GTK_IS_COLUMN_VIEW(event_widget) ? event_widget : gtk_widget_get_ancestor(event_widget, GTK_TYPE_COLUMN_VIEW);
	if (!view) return;
	const graphene_point_t point{static_cast<float>(x), static_cast<float>(y)};
	graphene_point_t translated;
	if (!gtk_widget_compute_point(event_widget, view, &point, &translated)) return;
	const guint position = dupe_view_position_at(view, translated.x, translated.y);
	DupeItem *di = dupe_listview_item_at(view, position);

	auto *dw = static_cast<DupeWindow *>(data);
	dw->click_item = di;

	const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
	const GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

	if (button == GDK_BUTTON_SECONDARY)
		{
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

		/* right click menu */
		if (state & GDK_CONTROL_MASK && state & GDK_SHIFT_MASK)
			{
			dupe_display_stats(dw, di);
			gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
			return;
			}

		}

	if (!di)
		{
		if (button == GDK_BUTTON_SECONDARY)
			{
			if (view == dw->listview)
				dupe_menu_popup_main(dw);
			else
				dupe_menu_popup_second(dw);
			}
		return;
		}

	if (button == GDK_BUTTON_PRIMARY &&
	    n_press == 2)
		{
		dupe_menu_view(di, view, FALSE);
		}

	if (button == GDK_BUTTON_MIDDLE)
		{
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		return;
		}

	if (button == GDK_BUTTON_SECONDARY)
		{
		if (!dupe_listview_item_is_selected(di, view))
			{
			dupe_view_select_item(view, position, true);
			}

		if (view == dw->listview)
			dupe_menu_popup_main(dw);
		else
			dupe_menu_popup_second(dw);

		return;
		}

	if (button == GDK_BUTTON_PRIMARY &&
	    n_press == 1 &&
	    !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) &&
	    dupe_listview_item_is_selected(di, view))
		{
		/* this selection handled on release_cb */
		gtk_widget_grab_focus(view);
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		return;
		}
}

static void dupe_listview_release_cb(GtkGestureClick *gesture, gint, gdouble x, gdouble y, gpointer data)
{
	const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

	if (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_MIDDLE)
		{
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		return;
		}

	GtkWidget *event_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	GtkWidget *view = GTK_IS_COLUMN_VIEW(event_widget) ? event_widget : gtk_widget_get_ancestor(event_widget, GTK_TYPE_COLUMN_VIEW);
	if (!view) return;
	const graphene_point_t point{static_cast<float>(x), static_cast<float>(y)};
	graphene_point_t translated;
	if (!gtk_widget_compute_point(event_widget, view, &point, &translated)) return;
	const guint position = dupe_view_position_at(view, translated.x, translated.y);
	DupeItem *di = dupe_listview_item_at(view, position);

	auto *dw = static_cast<DupeWindow *>(data);

	if (button == GDK_BUTTON_MIDDLE)
		{
		if (di && dw->click_item == di)
			{
			auto *selection = dupe_view_selection(view);
			if (dupe_listview_item_is_selected(di, view)) gtk_selection_model_unselect_item(selection, position);
			else gtk_selection_model_select_item(selection, position, FALSE);
			}

		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		return;
		}

	const GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

	if (di && dw->click_item == di &&
	    !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) &&
	    dupe_listview_item_is_selected(di, view))
		{
		dupe_view_select_item(view, position, true);

		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		return;
		}
}

/*
 *-------------------------------------------------------------------
 * second set stuff
 *-------------------------------------------------------------------
 */

static void dupe_second_update_status(DupeWindow *dw)
{
	g_autofree gchar *buf = g_strdup_printf(_("%d files (set 2)"), g_list_length(dw->second_list));
	gtk_label_set_text(GTK_LABEL(dw->second_status_label), buf);
}

static void dupe_second_add(DupeWindow *dw, DupeItem *di)
{
	if (!di) return;
	di->second = TRUE;
	dw->second_list = g_list_prepend(dw->second_list, di);
	g_autoptr(DupeRow) row = dupe_row_new(di);
	g_list_store_append(dupe_view_store(dw->second_listview), row);
	dupe_view_sort(dw->second_listview);
	dupe_second_update_status(dw);
}

static void dupe_second_remove(DupeWindow *dw, DupeItem *di)
{
	const gint position = dupe_listview_find_item(dw->second_listview, di);
	if (position >= 0) dupe_view_remove(dw->second_listview, position);
	dw->second_list = g_list_remove(dw->second_list, di);
	dupe_second_update_status(dw);
}

static void dupe_second_clear(DupeWindow *dw)
{
	g_list_store_remove_all(dupe_view_store(dw->second_listview));
	g_list_free(dw->dupes);
	dw->dupes = nullptr;
	g_list_free_full(dw->second_list, reinterpret_cast<GDestroyNotify>(dupe_item_free));
	dw->second_list = nullptr;
	dupe_match_reset_list(dw->list);
	dupe_second_update_status(dw);
}

template<gboolean new_window>
static void dupe_second_menu_view_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (dw->click_item) dupe_menu_view(dw->click_item, dw->second_listview, new_window);
}

static void dupe_second_menu_select_all_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	gtk_selection_model_select_all(dupe_view_selection(dw->second_listview));
}

static void dupe_second_menu_select_none_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	gtk_selection_model_unselect_all(dupe_view_selection(dw->second_listview));
}

static void dupe_second_menu_remove_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_window_remove_selection(dw, dw->second_listview);
}

static void dupe_second_menu_clear_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_second_clear(dw);
	dupe_window_recompare(dw);
}

static GtkWidget *dupe_menu_popup_second(DupeWindow *dw)
{
	g_autoptr(GtkBuilder) builder = gtk_builder_new_from_resource(GQ_RESOURCE_PATH_UI "/menu-dupe-second.ui");
	GMenu *menu_model = G_MENU(gtk_builder_get_object(builder, "menu-dupe-second"));

	dupe_popup_menu_centered(menu_model, dw, dw->second_listview);

	return nullptr;
}

static void dupe_second_set_toggle_cb(GtkWidget *widget, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dw->second_set = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));

	if (dw->second_set)
		{
		dupe_second_update_status(dw);
		gtk_widget_set_visible(dw->second_vbox, TRUE);
		}
	else
		{
		gtk_widget_set_visible(dw->second_vbox, FALSE);
		dupe_second_clear(dw);
		}

	dupe_window_recompare(dw);
}

static void dupe_ignore_same_folder_toggle_cb(GtkCheckButton *button, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	// Workers must finish before changing the comparison policy they read.
	dupe_check_stop(dw);
	dw->ignore_same_folder = gtk_check_button_get_active(button);
	dupe_window_recompare(dw);
}

static void dupe_sort_totals_toggle_cb(GtkWidget *widget, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	options->sort_totals = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
	dupe_window_recompare(dw);

}

/*
 *-------------------------------------------------------------------
 * match type menu
 *-------------------------------------------------------------------
 */

#define DUPE_MATCH_TYPE_KEY "dupe-match-type"
#define SIM_CUSTOM_INDEX_KEY "sim-custom-index"

static void dupe_listview_show_rank(GtkWidget *listview, gboolean rank);

static void dupe_menu_type_cb(GtkDropDown *drop_down, GParamSpec *, gpointer data)
{
	GObject *item = G_OBJECT(gtk_drop_down_get_selected_item(drop_down));
	if (!item) return;

	auto *dw = static_cast<DupeWindow *>(data);
	dw->match_mask = static_cast<DupeMatchType>(GPOINTER_TO_INT(g_object_get_data(item, DUPE_MATCH_TYPE_KEY)));

	options->duplicates_match = dw->match_mask;

	gtk_widget_set_sensitive(dw->algorithm_drop_down, dw->match_mask & DUPE_MATCH_SIM);
	gtk_widget_set_sensitive(dw->custom_threshold, dw->match_mask == DUPE_MATCH_SIM_CUSTOM);
	dupe_listview_show_rank(dw->listview, dw->match_mask & DUPE_MATCH_SIM);
	dupe_window_recompare(dw);
}

static GtkWidget *dupe_menu_setup(DupeWindow *dw)
{
	GtkStringList *string_list = gtk_string_list_new(nullptr);
	guint index = 0;
	guint current = GTK_INVALID_LIST_POSITION;
	const auto dupe_menu_add_item = [string_list, dw, &index, &current](const gchar *text, DupeMatchType type)
	{
		gtk_string_list_append(string_list, text);

		g_autoptr(GObject) item = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(string_list), index++));
		g_object_set_data(item, DUPE_MATCH_TYPE_KEY, GINT_TO_POINTER(type));

		if (dw->match_mask == type) current = index - 1;
	};

	dupe_menu_add_item(_("Name"), DUPE_MATCH_NAME);
	dupe_menu_add_item(_("Name case-insensitive"), DUPE_MATCH_NAME_CI);
	dupe_menu_add_item(_("Size"), DUPE_MATCH_SIZE);
	dupe_menu_add_item(_("Date"), DUPE_MATCH_DATE);
	dupe_menu_add_item(_("Dimensions"), DUPE_MATCH_DIM);
	dupe_menu_add_item(_("Checksum"), DUPE_MATCH_SUM);
	dupe_menu_add_item(_("Path"), DUPE_MATCH_PATH);
	dupe_menu_add_item(_("Similarity (high - 95)"), DUPE_MATCH_SIM_HIGH);
	dupe_menu_add_item(_("Similarity (med. - 90)"), DUPE_MATCH_SIM_MED);
	dupe_menu_add_item(_("Similarity (low - 85)"), DUPE_MATCH_SIM_LOW);
	dupe_menu_add_item(_("Similarity (custom)"), DUPE_MATCH_SIM_CUSTOM);

	const guint sim_custom_index = g_list_model_get_n_items(G_LIST_MODEL(string_list)) - 1;

	dupe_menu_add_item(_("Name ≠ content"), DUPE_MATCH_NAME_CONTENT);
	dupe_menu_add_item(_("Name case-insensitive ≠ content"), DUPE_MATCH_NAME_CI_CONTENT);
	dupe_menu_add_item(_("Show all"), DUPE_MATCH_ALL);

	GtkWidget *drop_down = gtk_drop_down_new(G_LIST_MODEL(string_list), nullptr);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(drop_down), current);

	g_signal_connect(G_OBJECT(drop_down), "notify::selected",
	                 G_CALLBACK(dupe_menu_type_cb), dw);
	g_object_set_data(G_OBJECT(drop_down), SIM_CUSTOM_INDEX_KEY, GUINT_TO_POINTER(sim_custom_index));
	return drop_down;
}

/*
 *-------------------------------------------------------------------
 * list view columns
 *-------------------------------------------------------------------
 */

static gint dupe_column_compare(gconstpointer a, gconstpointer b, gpointer data)
{
	const auto *first = static_cast<const DupeRow *>(a);
	const auto *second = static_cast<const DupeRow *>(b);
	const auto compare = [](auto x, auto y) { return x < y ? -1 : (x > y ? 1 : 0); };
	switch (GPOINTER_TO_INT(data))
		{
		case DUPE_COLUMN_NAME: return utf8_compare(first->fd->name, second->fd->name, TRUE);
		case DUPE_COLUMN_SIZE: return compare(first->fd->size, second->fd->size);
		case DUPE_COLUMN_DATE: return compare(first->fd->date, second->fd->date);
		case DUPE_COLUMN_SET: return compare(first->group, second->group);
		case DUPE_COLUMN_DIMENSIONS: return compare(first->dimensions.area(), second->dimensions.area());
		case DUPE_COLUMN_PATH: return utf8_compare(first->fd->path, second->fd->path, TRUE);
		case DUPE_COLUMN_RANK:
			{
			const gint first_rank = atoi(first->rank);
			const gint second_rank = atoi(second->rank);
			return compare(first_rank ? first_rank : 101, second_rank ? second_rank : 101);
			}
		default: return 0;
		}
}

static void dupe_cell_update(DupeRow *row, GtkWidget *child)
{
	const gint column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "dupe-column"));
	GtkWidget *content = gtk_widget_get_first_child(child);
	if (row->color) gtk_widget_add_css_class(child, "dupe-group");
	else gtk_widget_remove_css_class(child, "dupe-group");
	if (column == DUPE_COLUMN_THUMB)
		{
		gtk_widget_set_size_request(content, options->thumbnails.size.width, options->thumbnails.size.height);
		g_autoptr(GdkTexture) texture = row->thumb ? pixbuf_to_texture(row->thumb) : nullptr;
		gtk_picture_set_paintable(GTK_PICTURE(content), GDK_PAINTABLE(texture));
		return;
		}
	g_autofree gchar *allocated = nullptr;
	const gchar *text = "";
	switch (column)
		{
		case DUPE_COLUMN_RANK: text = row->rank; break;
		case DUPE_COLUMN_NAME: text = row->fd->name; break;
		case DUPE_COLUMN_SIZE: allocated = text_from_size(row->fd->size); text = allocated; break;
		case DUPE_COLUMN_DATE: text = text_from_time(row->fd->date); break;
		case DUPE_COLUMN_PATH: text = row->fd->path; break;
		case DUPE_COLUMN_DIMENSIONS:
			if (!row->dimensions.empty()) allocated = g_strdup_printf("%d x %d", row->dimensions.width, row->dimensions.height);
			text = allocated;
			break;
		case DUPE_COLUMN_SET: allocated = g_strdup_printf("%d", row->group); text = allocated; break;
		default: break;
		}
	gtk_label_set_text(GTK_LABEL(content), text ? text : "");
}

static void dupe_cell_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
	const gint column = GPOINTER_TO_INT(data);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	g_object_set_data(G_OBJECT(box), "dupe-list-item", item);
	g_object_set_data(G_OBJECT(box), "dupe-column", data);
	gtk_widget_set_hexpand(box, TRUE);
	gtk_widget_set_vexpand(box, TRUE);
	GtkWidget *content;
	if (column == DUPE_COLUMN_THUMB)
		{
		content = gtk_picture_new();
		gtk_picture_set_can_shrink(GTK_PICTURE(content), TRUE);
		gtk_picture_set_content_fit(GTK_PICTURE(content), GTK_CONTENT_FIT_CONTAIN);
		}
	else
		{
		content = gtk_label_new(nullptr);
		gtk_label_set_xalign(GTK_LABEL(content), column == DUPE_COLUMN_RANK || column == DUPE_COLUMN_SIZE || column == DUPE_COLUMN_DATE ? 1.0F : 0.0F);
		gtk_label_set_ellipsize(GTK_LABEL(content), PANGO_ELLIPSIZE_END);
		gtk_widget_set_hexpand(content, TRUE);
		}
	gtk_widget_set_margin_start(content, 4);
	gtk_widget_set_margin_end(content, 4);
	gtk_box_append(GTK_BOX(box), content);
	gtk_list_item_set_child(item, box);
	auto *dw = static_cast<DupeWindow *>(g_object_get_data(G_OBJECT(factory), "dupe-window"));
	auto *drag = gtk_drag_source_new();
	gtk_drag_source_set_actions(drag, static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
	g_signal_connect(drag, "prepare", G_CALLBACK(dupe_dnd_prepare), dw);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));
	auto *gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
	g_signal_connect(gesture, "pressed", G_CALLBACK(dupe_listview_press_cb), dw);
	g_signal_connect(gesture, "released", G_CALLBACK(dupe_listview_release_cb), dw);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(gesture));
	gtk_gesture_group(gesture, GTK_GESTURE(drag));
}

static void dupe_cell_bind(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
	auto *row = static_cast<DupeRow *>(gtk_list_item_get_item(item));
	GtkWidget *child = gtk_list_item_get_child(item);
	g_signal_connect(row, "changed", G_CALLBACK(dupe_cell_update), child);
	dupe_cell_update(row, child);
}

static void dupe_cell_unbind(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
	auto *row = static_cast<DupeRow *>(gtk_list_item_get_item(item));
	if (row) g_signal_handlers_disconnect_by_data(row, gtk_list_item_get_child(item));
}

static GtkWidget *dupe_view_new()
{
	auto *store = g_list_store_new(dupe_row_get_type());
	auto *selection = gtk_multi_selection_new(G_LIST_MODEL(store));
	GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(selection));
	gtk_widget_add_css_class(view, "geeqie-file-list");
	g_signal_connect_object(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(view)), "changed",
	                        G_CALLBACK(+[](GtkSorter *, GtkSorterChange, gpointer data)
	                        { dupe_view_sort(GTK_WIDGET(data)); }), view, G_CONNECT_DEFAULT);
	return view;
}

static void dupe_listview_add_column(DupeWindow *dw, GtkWidget *view, gint n, const gchar *title, gboolean image)
{
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_object_set_data(G_OBJECT(factory), "dupe-window", dw);
	g_signal_connect(factory, "setup", G_CALLBACK(dupe_cell_setup), GINT_TO_POINTER(n));
	g_signal_connect(factory, "bind", G_CALLBACK(dupe_cell_bind), nullptr);
	g_signal_connect(factory, "unbind", G_CALLBACK(dupe_cell_unbind), nullptr);
	g_autoptr(GtkColumnViewColumn) column = gtk_column_view_column_new(title, factory);
	g_object_set_data(G_OBJECT(column), "dupe-column", GINT_TO_POINTER(n));
	gtk_column_view_column_set_resizable(column, !image && n != DUPE_COLUMN_RANK);
	gtk_column_view_column_set_fixed_width(column, image ? 4 : -1);
	if (n != DUPE_COLUMN_THUMB)
		{
		g_autoptr(GtkCustomSorter) sorter = gtk_custom_sorter_new(dupe_column_compare, GINT_TO_POINTER(n), nullptr);
		gtk_column_view_column_set_sorter(column, GTK_SORTER(sorter));
		}
	gtk_column_view_append_column(GTK_COLUMN_VIEW(view), column);
}

static GtkColumnViewColumn *dupe_view_column(GtkWidget *view, gint id)
{
	auto *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(view));
	for (guint i = 0; i < g_list_model_get_n_items(columns); ++i)
		{
		auto *column = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(columns, i));
		if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "dupe-column")) == id) return column;
		g_object_unref(column);
		}
	return nullptr;
}

static void dupe_listview_set_height(GtkWidget *view, gboolean thumb)
{
	g_autoptr(GtkColumnViewColumn) column = dupe_view_column(view, DUPE_COLUMN_THUMB);
	if (!column) return;
	gtk_column_view_column_set_fixed_width(column, thumb ? options->thumbnails.size.width + 8 : 4);
	gtk_column_view_column_set_visible(column, thumb);
}

static void dupe_listview_show_rank(GtkWidget *view, gboolean rank)
{
	g_autoptr(GtkColumnViewColumn) column = dupe_view_column(view, DUPE_COLUMN_RANK);
	if (column) gtk_column_view_column_set_visible(column, rank);
}

/*
 *-------------------------------------------------------------------
 * misc cb
 *-------------------------------------------------------------------
 */

static void dupe_window_show_thumb_cb(GtkWidget *widget, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dw->show_thumbs = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
	options->duplicates_thumbnails = dw->show_thumbs;

	if (dw->show_thumbs)
		{
		if (!dw->working) dupe_thumb_step(dw);
		}
	else
		{
		thumb_loader_free(dw->thumb_loader);
		dw->thumb_loader = nullptr;
		dw->thumb_item = nullptr;
		const guint count = g_list_model_get_n_items(G_LIST_MODEL(dupe_view_store(dw->listview)));
		for (guint i = 0; i < count; ++i)
			{
			g_autoptr(DupeRow) row = dupe_view_row(dw->listview, i);
			g_clear_object(&row->thumb);
			g_signal_emit_by_name(row, "changed");
			}
		dupe_window_update_progress(dw, nullptr, 0.0, FALSE);
		}

	dupe_listview_set_height(dw->listview, dw->show_thumbs);
}

static void dupe_algorithm_label_setup(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
	gtk_list_item_set_child(item, gtk_label_new(nullptr));
}

static void dupe_algorithm_label_bind(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
	const gboolean perceptual = gtk_list_item_get_position(item) == 1;
	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), perceptual ? _("Perceptual") : _("Standard"));
}

static void dupe_algorithm_list_label_bind(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
	auto *algorithm = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), gtk_string_object_get_string(algorithm));
}

void dupe_set_similarity_algorithm(gboolean perceptual)
{
	if (options->alternate_similarity_algorithm.enabled == perceptual) return;

	// Drain comparisons before changing the algorithm shared by all windows.
	for (GList *work = dupe_window_list; work; work = work->next)
		{
		dupe_check_stop(static_cast<DupeWindow *>(work->data));
		}
	options->alternate_similarity_algorithm.enabled = perceptual;
	for (GList *work = dupe_window_list; work; work = work->next)
		{
		auto *dw = static_cast<DupeWindow *>(work->data);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(dw->algorithm_drop_down), perceptual ? 1 : 0);
		dupe_window_recompare(dw);
		}
}

static void dupe_window_algorithm_cb(GtkDropDown *drop_down, GParamSpec *, gpointer)
{
	dupe_set_similarity_algorithm(gtk_drop_down_get_selected(drop_down) == 1);
}

static void dupe_window_rotation_invariant_cb(GtkWidget *widget, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	options->rot_invariant_sim = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget));
	dupe_window_recompare(dw);
}

static void dupe_window_custom_threshold_cb(GtkSpinButton *custom_threshold, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	options->duplicates_similarity_threshold = gtk_spin_button_get_value_as_int(custom_threshold);
	dw->match_mask = DUPE_MATCH_SIM_CUSTOM;

	const guint sim_custom_index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dw->drop_down),
	                                                                  SIM_CUSTOM_INDEX_KEY));

	gtk_drop_down_set_selected(GTK_DROP_DOWN(dw->drop_down), sim_custom_index);
	dupe_window_recompare(dw);
}

void dupe_window_clear(DupeWindow *dw)
{
	dupe_check_stop(dw);
	g_list_store_remove_all(dupe_view_store(dw->listview));
	dupe_view_sort(dw->listview);

	g_list_free(dw->dupes);
	dw->dupes = nullptr;

	g_list_free_full(dw->list, reinterpret_cast<GDestroyNotify>(dupe_item_free));
	dw->list = nullptr;
	dw->set_count = 0;

	dupe_match_reset_list(dw->second_list);

	dupe_window_update_count(dw, FALSE);
	dupe_window_update_progress(dw, nullptr, 0.0, FALSE);
}

static void dupe_window_get_geometry(DupeWindow *dw)
{
	LayoutWindow *lw = get_current_layout();
	if (!dw || !lw) return;

	lw->options.dupe_window.rect = widget_get_position_geometry(dw->window);
	lw->options.dupe_window.vdivider_pos = gtk_paned_get_position(GTK_PANED(dw->paned));
}

void dupe_window_close(DupeWindow *dw)
{
	g_clear_handle_id(&dw->delete_update_id, g_source_remove);
	g_clear_pointer(&dw->pending_deletions, g_hash_table_destroy);
	dupe_check_stop(dw);

	dupe_window_get_geometry(dw);

	dupe_window_list = g_list_remove(dupe_window_list, dw);
	g_object_set_data(G_OBJECT(dw->window), DUPE_WINDOW_DATA_KEY, nullptr);
	gtk_window_destroy(GTK_WINDOW(dw->window));

	g_list_free(dw->dupes);
	g_list_free_full(dw->list, reinterpret_cast<GDestroyNotify>(dupe_item_free));

	g_list_free_full(dw->second_list, reinterpret_cast<GDestroyNotify>(dupe_item_free));

	file_data_unregister_notify_func(dupe_notify_cb, dw);

	g_thread_pool_free(dw->dupe_comparison_thread_pool, TRUE, TRUE);

	g_free(dw);
}

static void dupe_window_close_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_window_close(dw);
}

static void dupe_window_close_button_cb(GtkWidget *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	dupe_window_close(dw);
}

static gint dupe_window_delete(GtkWidget *, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);
	dupe_window_close(dw);

	return TRUE;
}

static void dupe_help_cb(GSimpleAction *, GVariant *, gpointer)
{
	help_window_show("GuideImageSearchFindingDuplicates.html"); // NOTRANSLATE: Documentation filename.
}



enum SeparatorType {
	EXPORT_CSV = 0,
	EXPORT_TSV,
	EXPORT_JSON
};

struct ExportDupesData
{
	GString *output;
};


static GString *export_duplicates_data(DupeWindow *dw, const char *sep)
{
	bool color_old = FALSE;
	bool color_new = FALSE;
	int match_count;

	g_autofree char *header = g_strjoin(sep, _("Match"), _("Group"), _("Similarity"), _("Set"), _("Thumbnail"), _("Name"), _("Size"), _("Date"), _("Width"), _("Height"), _("Path"), NULL);

	GString *output_string = g_string_new(header);
	output_string = g_string_append_c(output_string, '\n');

	auto *selection = dupe_view_selection(dw->listview);
	g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(selection);
	if (gtk_bitset_is_empty(selected)) return output_string;
	g_autoptr(DupeRow) first = dupe_view_row(dw->listview, gtk_bitset_get_minimum(selected));
	color_old = !first->color;
	match_count = 0;
	GtkBitsetIter iter;
	guint position;
	for (bool valid = gtk_bitset_iter_init_first(&iter, selected, &position); valid; valid = gtk_bitset_iter_next(&iter, &position))
		{
		g_autoptr(DupeRow) row = dupe_view_row(dw->listview, position);
		auto *di = row->item;
		color_new = row->color;
		if (color_new != color_old)
			{
			match_count++;
			}
		color_old = color_new;
		g_string_append_printf(output_string, "%d", match_count);
		output_string = g_string_append(output_string, sep);

		if ((dupe_match_find_parent(dw, di) == di))
			{
			output_string = g_string_append(output_string, "1");
			}
		else
			{
			output_string = g_string_append(output_string, "2");
			}
		output_string = g_string_append(output_string, sep);

		const char *rank = row->rank;
		g_auto(GStrv) rank_split = g_strsplit_set(rank, " [(", -1);
		if (rank_split[0] == nullptr)
			{
			output_string = g_string_append(output_string, "");
			}
		else
			{
			output_string = g_string_append(output_string, rank_split[0]);
			}
		output_string = g_string_append(output_string, sep);

		g_string_append_printf(output_string, "%d", di->second + 1);
		output_string = g_string_append(output_string, sep);

		g_autofree char *thumb_cache = cache_find_location(CacheType::THUMB, di->fd->path);
		output_string = g_string_append(output_string, thumb_cache ? thumb_cache : "");
		output_string = g_string_append(output_string, sep);

		const char *name = row->fd->name;
		output_string = g_string_append(output_string, name);
		output_string = g_string_append(output_string, sep);

		g_string_append_printf(output_string, "%" PRId64, di->fd->size);
		output_string = g_string_append(output_string, sep);
		output_string = g_string_append(output_string, text_from_time(di->fd->date));
		output_string = g_string_append(output_string, sep);
		g_string_append_printf(output_string, "%d", di->dimensions.width);
		output_string = g_string_append(output_string, sep);
		g_string_append_printf(output_string, "%d", di->dimensions.height);
		output_string = g_string_append(output_string, sep);
		output_string = g_string_append(output_string, di->fd->path);
		output_string = g_string_append_c(output_string, '\n');
		}

	return output_string;
}

/*
 *-------------------------------------------------------------------
 * Export duplicates data
 *-------------------------------------------------------------------
 */
GString *export_duplicates_data_command_line()
{
	if (!dupe_window_list) return g_string_new(_("No duplicates windows open"));

	auto *dw = static_cast<DupeWindow *>(g_list_last(dupe_window_list)->data);

	if (dw->idle_id != 0) return g_string_new(_("Incomplete"));

	gtk_selection_model_select_all(dupe_view_selection(dw->listview));

	return export_duplicates_data(dw, "\t");
}

GString *export_duplicates_json_command_line()
{
	auto *dw = dupe_window_list ? static_cast<DupeWindow *>(g_list_last(dupe_window_list)->data) : nullptr;
	g_autoptr(FileDataList) selection = dw ? dupe_listview_get_selection(dw->listview) : nullptr;
	return dupe_export_json(dw, selection);
}

static GString *dupe_json_snapshot(DupeWindow *dw)
{
	if (!dupe_export_ready(dw))
		{
		file_util_warning_dialog(_("Comparison incomplete"), _("Wait for the duplicate comparison to finish before exporting results."),
		                         GQ_ICON_DIALOG_WARNING, dw->window);
		return nullptr;
		}
	g_autoptr(FileDataList) selection = dupe_listview_get_selection(dw->listview);
	return dupe_export_json(dw, selection);
}

static void save_export_file(GFile *file, ExportDupesData *edd)
{
	g_autoptr(GError) error = nullptr;
	g_autoptr(GFileOutputStream) gfstream = g_file_replace(file, nullptr, TRUE, G_FILE_CREATE_NONE, nullptr, &error);
	if (error)
		{
		file_util_warning_dialog(_("Export failed"), error->message, GQ_ICON_DIALOG_ERROR, nullptr);
		return;
		}

	if (!g_output_stream_write_all(G_OUTPUT_STREAM(gfstream), edd->output->str, edd->output->len, nullptr, nullptr, &error) ||
	    !g_output_stream_close(G_OUTPUT_STREAM(gfstream), nullptr, &error))
		file_util_warning_dialog(_("Export failed"), error->message, GQ_ICON_DIALOG_ERROR, nullptr);

	g_autoptr(GFile) parent = g_file_get_parent(file);
	if (!parent) return;

	g_autofree char *parent_path = g_file_get_path(parent);
	if (parent_path) history_list_add_to_key("export_duplicates", parent_path, -1);
}

static void export_response_cb(GFile *file, gpointer data)
{
	auto edd = static_cast<ExportDupesData *>(data);

	if (file != nullptr)
		{
		save_export_file(file, edd);
		}

	g_string_free(edd->output, TRUE);
	g_free(edd);
}




template<SeparatorType separator>
static void dupe_pop_menu_export_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);

	auto *edd = g_new0(ExportDupesData, 1);
	edd->output = separator == EXPORT_JSON ? dupe_json_snapshot(dw) :
	              export_duplicates_data(dw, separator == EXPORT_CSV ? "," : "\t");
	if (!edd->output)
		{
		g_free(edd);
		return;
		}

	FileDialogData fdd{};

	fdd.action = FileDialogAction::SAVE;
	fdd.accept_text = _("Save");
	fdd.callback = export_response_cb;
	fdd.data = edd;
	fdd.filter = separator == EXPORT_JSON ? ".json" : separator == EXPORT_CSV ? ".csv" : ".tsv";
	fdd.filter_description = separator == EXPORT_JSON ? _("JSON files") : separator == EXPORT_CSV ? _("CSV files") : _("TSV files");
	fdd.history_key = "export_duplicates";
	fdd.suggested_name = separator == EXPORT_JSON ? _("Untitled.json") : separator == EXPORT_CSV ? _("Untitled.csv") : _("Untitled.tsv");
	fdd.title = _("Export duplicates data");

	file_dialog_show(fdd);
}

static void dupe_script_finished_cb(GObject *, GAsyncResult *result, gpointer)
{
	g_autoptr(GError) error = nullptr;
	if (!dupe_run_results_script_finish(result, &error))
		file_util_warning_dialog(_("Script failed"), error->message, GQ_ICON_DIALOG_ERROR, nullptr);
}

static void dupe_script_selected_cb(GFile *file, gpointer data)
{
	g_autoptr(GString) contents = static_cast<GString *>(data);
	if (file) dupe_run_results_script(file, contents->str, dupe_script_finished_cb, nullptr);
}

static void dupe_run_script_cb(GSimpleAction *, GVariant *, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	GString *contents = dupe_json_snapshot(dw);
	if (!contents) return;
	FileDialogData fdd{};
	fdd.action = FileDialogAction::OPEN;
	fdd.title = _("Run script with duplicate results");
	fdd.accept_text = _("Run");
	fdd.history_key = "duplicates_script";
	fdd.callback = dupe_script_selected_cb;
	fdd.data = contents;
	fdd.parent = GTK_WINDOW(dw->window);
	file_dialog_show(fdd);
}

/* static const ActionDef dupe_main_actions[]
 */
#include "dupe-actions.inc"

/* collection and files can be NULL */
DupeWindow *dupe_window_new()
{
	DupeWindow *dw;
	GtkWidget *vbox;
	GtkWidget *hbox;
	GtkWidget *scrolled;
	GtkWidget *frame;
	GtkWidget *status_box;
	GtkWidget *controls_box;
	GtkWidget *button_box;
	GtkWidget *label;
	GtkWidget *button;

	dw = g_new0(DupeWindow, 1);
	dw->add_files_queue = nullptr;
	dw->add_files_queue_id = 0;

	dw->match_mask = DUPE_MATCH_NAME;
	if (options->duplicates_match == DUPE_MATCH_NAME) dw->match_mask = DUPE_MATCH_NAME;
	if (options->duplicates_match == DUPE_MATCH_SIZE) dw->match_mask = DUPE_MATCH_SIZE;
	if (options->duplicates_match == DUPE_MATCH_DATE) dw->match_mask = DUPE_MATCH_DATE;
	if (options->duplicates_match == DUPE_MATCH_DIM) dw->match_mask = DUPE_MATCH_DIM;
	if (options->duplicates_match == DUPE_MATCH_SUM) dw->match_mask = DUPE_MATCH_SUM;
	if (options->duplicates_match == DUPE_MATCH_PATH) dw->match_mask = DUPE_MATCH_PATH;
	if (options->duplicates_match == DUPE_MATCH_SIM_HIGH) dw->match_mask = DUPE_MATCH_SIM_HIGH;
	if (options->duplicates_match == DUPE_MATCH_SIM_MED) dw->match_mask = DUPE_MATCH_SIM_MED;
	if (options->duplicates_match == DUPE_MATCH_SIM_LOW) dw->match_mask = DUPE_MATCH_SIM_LOW;
	if (options->duplicates_match == DUPE_MATCH_SIM_CUSTOM) dw->match_mask = DUPE_MATCH_SIM_CUSTOM;
	if (options->duplicates_match == DUPE_MATCH_NAME_CI) dw->match_mask = DUPE_MATCH_NAME_CI;
	if (options->duplicates_match == DUPE_MATCH_NAME_CONTENT) dw->match_mask = DUPE_MATCH_NAME_CONTENT;
	if (options->duplicates_match == DUPE_MATCH_NAME_CI_CONTENT) dw->match_mask = DUPE_MATCH_NAME_CI_CONTENT;
	if (options->duplicates_match == DUPE_MATCH_ALL) dw->match_mask = DUPE_MATCH_ALL;

	dw->window = window_new("dupe", nullptr, _("Find duplicates"));
	DEBUG_NAME(dw->window);
	g_object_set_data(G_OBJECT(dw->window), DUPE_WINDOW_DATA_KEY, dw);

	gtk_widget_set_size_request(dw->window, DEFAULT_MINIMAL_WINDOW_SIZE, DEFAULT_MINIMAL_WINDOW_SIZE);

	LayoutWindow *lw = get_current_layout();
	if (lw && options->save_window_positions)
		{
		gtk_window_set_default_size(GTK_WINDOW(dw->window), lw->options.dupe_window.rect.width, lw->options.dupe_window.rect.height);
		}
	else
		{
		gtk_window_set_default_size(GTK_WINDOW(dw->window), DUPE_DEF_WIDTH, DUPE_DEF_HEIGHT);
		}

	gtk_window_set_resizable(GTK_WINDOW(dw->window), TRUE);
	gtk_widget_set_margin_top(dw->window, 0);
	gtk_widget_set_margin_bottom(dw->window, 0);
	gtk_widget_set_margin_start(dw->window, 0);
	gtk_widget_set_margin_end(dw->window, 0);

	g_signal_connect(G_OBJECT(dw->window), "close-request",
			 G_CALLBACK(dupe_window_delete), dw);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(GTK_WINDOW(dw->window), vbox);

	dw->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	if (lw && options->save_window_positions && (lw->options.dupe_window.vdivider_pos != 0))
		{
		gtk_paned_set_position(GTK_PANED(dw->paned), lw->options.dupe_window.vdivider_pos);
		}
	gtk_widget_set_hexpand(dw->paned, gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(vbox))) == GTK_ORIENTATION_HORIZONTAL ? TRUE : FALSE);
	gtk_widget_set_vexpand(dw->paned, gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(vbox))) == GTK_ORIENTATION_VERTICAL ? TRUE : FALSE);
	gtk_box_append(GTK_BOX(vbox), dw->paned);

	scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scrolled), true);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	dw->listview = dupe_view_new();

	dupe_listview_add_column(dw, dw->listview, DUPE_COLUMN_RANK, _("Rank"), FALSE);
	dupe_listview_add_column(dw, dw->listview, DUPE_COLUMN_THUMB, _("Thumb"), TRUE);
	dupe_listview_add_column(dw, dw->listview, DUPE_COLUMN_NAME, _("Name"), FALSE);
	dupe_listview_add_column(dw, dw->listview, DUPE_COLUMN_SIZE, _("Size"), FALSE);
	dupe_listview_add_column(dw, dw->listview, DUPE_COLUMN_DATE, _("Date"), FALSE);
	dupe_listview_add_column(dw, dw->listview, DUPE_COLUMN_DIMENSIONS, _("Dimensions"), FALSE);
	dupe_listview_add_column(dw, dw->listview, DUPE_COLUMN_PATH, _("Path"), FALSE);
	dupe_listview_add_column(dw, dw->listview, DUPE_COLUMN_SET, _("Set"), FALSE);

	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dw->listview);

	dw->second_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_visible(dw->second_vbox, dw->second_set);

	gtk_paned_set_start_child(GTK_PANED(dw->paned), scrolled);
	gtk_paned_set_end_child(GTK_PANED(dw->paned), dw->second_vbox);

	scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scrolled), true);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand(scrolled, gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(dw->second_vbox))) == GTK_ORIENTATION_HORIZONTAL ? TRUE : FALSE);
	gtk_widget_set_vexpand(scrolled, gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(dw->second_vbox))) == GTK_ORIENTATION_VERTICAL ? TRUE : FALSE);
	gtk_box_append(GTK_BOX(dw->second_vbox), scrolled);

	dw->second_listview = dupe_view_new();
	dupe_listview_add_column(dw, dw->second_listview, DUPE_COLUMN_PATH, _("Compare to:"), FALSE);

	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dw->second_listview);

	dw->second_status_label = gtk_label_new("");
	gtk_box_append(GTK_BOX(dw->second_vbox), dw->second_status_label);

	pref_line(dw->second_vbox, GTK_ORIENTATION_HORIZONTAL);

	status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(vbox), status_box);

	frame = gtk_frame_new(nullptr);
	DEBUG_NAME(frame);
	gtk_widget_add_css_class(frame, "frame");
	gtk_widget_set_hexpand(frame, gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(status_box))) == GTK_ORIENTATION_HORIZONTAL ? TRUE : FALSE);
	gtk_widget_set_vexpand(frame, gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(status_box))) == GTK_ORIENTATION_VERTICAL ? TRUE : FALSE);
	gtk_box_append(GTK_BOX(status_box), frame);

	dw->status_label = gtk_label_new("");
	gtk_frame_set_child(GTK_FRAME(frame), dw->status_label);

	dw->extra_label = gtk_progress_bar_new();
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dw->extra_label), 0.0);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dw->extra_label), "");
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(dw->extra_label), TRUE);
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(status_box))) == GTK_ORIENTATION_HORIZONTAL)
		{
		gtk_widget_set_margin_end(dw->extra_label, PREF_PAD_SPACE);
		}
	else
		{
		gtk_widget_set_margin_bottom(dw->extra_label, PREF_PAD_SPACE);
		}
	gtk_box_append(GTK_BOX(status_box), dw->extra_label);

	dw->controls_box = pref_box_new(vbox, FALSE, GTK_ORIENTATION_VERTICAL, 0);
	controls_box = pref_box_new(dw->controls_box, FALSE, GTK_ORIENTATION_HORIZONTAL, 0);

	dw->button_thumbs = gtk_check_button_new_with_label(_("Thumbnails"));
	g_autofree gchar *thumbs_accel = action_accelerator_label("win.dupe-win-toggle-thumbnails");
	gtk_widget_set_tooltip_text(dw->button_thumbs, thumbs_accel);
	dw->show_thumbs = options->duplicates_thumbnails;
	gtk_check_button_set_active(GTK_CHECK_BUTTON(dw->button_thumbs), dw->show_thumbs);
	g_signal_connect(G_OBJECT(dw->button_thumbs), "toggled",
			 G_CALLBACK(dupe_window_show_thumb_cb), dw);
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(controls_box))) == GTK_ORIENTATION_HORIZONTAL)
		{
		gtk_widget_set_margin_end(dw->button_thumbs, PREF_PAD_SPACE);
		}
	else
		{
		gtk_widget_set_margin_bottom(dw->button_thumbs, PREF_PAD_SPACE);
		}
	gtk_box_append(GTK_BOX(controls_box), dw->button_thumbs);

	label = gtk_label_new(_("Compare by:"));
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(controls_box))) == GTK_ORIENTATION_HORIZONTAL)
		{
		gtk_widget_set_margin_end(label, PREF_PAD_SPACE);
		}
	else
		{
		gtk_widget_set_margin_bottom(label, PREF_PAD_SPACE);
		}
	gtk_box_append(GTK_BOX(controls_box), label);

	dw->drop_down = dupe_menu_setup(dw);
	gtk_box_append(GTK_BOX(controls_box), dw->drop_down);

	const gchar *algorithms[] = {_("Standard similarity algorithm"), _("Perceptual hash algorithm"), nullptr};
	dw->algorithm_drop_down = gtk_drop_down_new_from_strings(algorithms);
	auto *algorithm_factory = gtk_signal_list_item_factory_new();
	g_signal_connect(algorithm_factory, "setup", G_CALLBACK(dupe_algorithm_label_setup), nullptr);
	g_signal_connect(algorithm_factory, "bind", G_CALLBACK(dupe_algorithm_label_bind), nullptr);
	gtk_drop_down_set_factory(GTK_DROP_DOWN(dw->algorithm_drop_down), algorithm_factory);
	g_object_unref(algorithm_factory);
	// Keep full algorithm names in the popup and short names on the button.
	auto *algorithm_list_factory = gtk_signal_list_item_factory_new();
	g_signal_connect(algorithm_list_factory, "setup", G_CALLBACK(dupe_algorithm_label_setup), nullptr);
	g_signal_connect(algorithm_list_factory, "bind", G_CALLBACK(dupe_algorithm_list_label_bind), nullptr);
	gtk_drop_down_set_list_factory(GTK_DROP_DOWN(dw->algorithm_drop_down), algorithm_list_factory);
	g_object_unref(algorithm_list_factory);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dw->algorithm_drop_down), options->alternate_similarity_algorithm.enabled ? 1 : 0);
	gtk_widget_set_tooltip_text(dw->algorithm_drop_down, _("Perceptual hash can be significantly faster than the standard algorithm. Similarity percentages differ between algorithms; verify the threshold with your images before relying on the results."));
	g_signal_connect(dw->algorithm_drop_down, "notify::selected", G_CALLBACK(dupe_window_algorithm_cb), nullptr);
	gtk_widget_set_margin_end(dw->algorithm_drop_down, PREF_PAD_SPACE);
	gtk_box_append(GTK_BOX(controls_box), dw->algorithm_drop_down);

	label = gtk_label_new(_("Custom Threshold"));
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(controls_box))) == GTK_ORIENTATION_HORIZONTAL)
		{
		gtk_widget_set_margin_end(label, PREF_PAD_SPACE);
		}
	else
		{
		gtk_widget_set_margin_bottom(label, PREF_PAD_SPACE);
		}
	gtk_box_append(GTK_BOX(controls_box), label);
	dw->custom_threshold = gtk_spin_button_new_with_range(1, 100, 1);
	gtk_widget_set_tooltip_text(dw->custom_threshold, _("Custom similarity threshold\n(Use tab key to set value)"));
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(dw->custom_threshold), options->duplicates_similarity_threshold);
	g_signal_connect(G_OBJECT(dw->custom_threshold), "value-changed", G_CALLBACK(dupe_window_custom_threshold_cb), dw);
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(controls_box))) == GTK_ORIENTATION_HORIZONTAL)
		{
		gtk_widget_set_margin_end(dw->custom_threshold, PREF_PAD_SPACE);
		}
	else
		{
		gtk_widget_set_margin_bottom(dw->custom_threshold, PREF_PAD_SPACE);
		}
	gtk_box_append(GTK_BOX(controls_box), dw->custom_threshold);

	button = gtk_check_button_new_with_label(_("Sort"));
	gtk_widget_set_tooltip_text(button, _("Sort by group totals"));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(button), options->sort_totals);
	g_signal_connect(G_OBJECT(button), "toggled", G_CALLBACK(dupe_sort_totals_toggle_cb), dw);
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(controls_box))) == GTK_ORIENTATION_HORIZONTAL)
		{
		gtk_widget_set_margin_end(button, PREF_PAD_SPACE);
		}
	else
		{
		gtk_widget_set_margin_bottom(button, PREF_PAD_SPACE);
		}
	gtk_box_append(GTK_BOX(controls_box), button);

	dw->button_rotation_invariant = gtk_check_button_new_with_label(_("Ignore Orientation"));
	gtk_widget_set_tooltip_text(dw->button_rotation_invariant, _("Ignore image orientation"));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(dw->button_rotation_invariant), options->rot_invariant_sim);
	g_signal_connect(G_OBJECT(dw->button_rotation_invariant), "toggled",
			 G_CALLBACK(dupe_window_rotation_invariant_cb), dw);
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(controls_box))) == GTK_ORIENTATION_HORIZONTAL)
		{
		gtk_widget_set_margin_end(dw->button_rotation_invariant, PREF_PAD_SPACE);
		}
	else
		{
		gtk_widget_set_margin_bottom(dw->button_rotation_invariant, PREF_PAD_SPACE);
		}
	gtk_box_append(GTK_BOX(controls_box), dw->button_rotation_invariant);

	dw->button_ignore_same_folder = gtk_check_button_new_with_label(_("Ignore matches within the same folder"));
	gtk_widget_set_tooltip_text(dw->button_ignore_same_folder, _("Compare files only when their immediate parent folders differ"));
	g_signal_connect(dw->button_ignore_same_folder, "toggled", G_CALLBACK(dupe_ignore_same_folder_toggle_cb), dw);
	gtk_box_append(GTK_BOX(dw->controls_box), dw->button_ignore_same_folder);

	button = gtk_check_button_new_with_label(_("Compare two file sets"));
	gtk_check_button_set_active(GTK_CHECK_BUTTON(button), dw->second_set);
	g_signal_connect(G_OBJECT(button), "toggled",
			 G_CALLBACK(dupe_second_set_toggle_cb), dw);
	if (gtk_orientable_get_orientation(GTK_ORIENTABLE(GTK_BOX(controls_box))) == GTK_ORIENTATION_HORIZONTAL)
		{
		gtk_widget_set_margin_end(button, PREF_PAD_SPACE);
		}
	else
		{
		gtk_widget_set_margin_bottom(button, PREF_PAD_SPACE);
		}
	gtk_box_append(GTK_BOX(controls_box), button);

	button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(vbox), button_box);
	gtk_widget_set_halign(button_box, GTK_ALIGN_END);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PREF_PAD_BUTTON_GAP);
	gtk_box_append(GTK_BOX(button_box), hbox);

	button = pref_button_new(hbox, GQ_ICON_HELP, _("Help"), G_CALLBACK(dupe_help_cb), nullptr);
	g_autofree gchar *help_accel = action_accelerator_label("win.dupe-win-help");
	gtk_widget_set_tooltip_text(button, help_accel);

	pref_button_new(hbox, GQ_ICON_STOP, _("Stop"), G_CALLBACK(dupe_check_stop_cb), dw);

	button = pref_button_new(hbox, GQ_ICON_CLOSE, _("Close"), G_CALLBACK(dupe_window_close_button_cb), dw);
	g_autofree gchar *close_accel = action_accelerator_label("win.dupe-win-window-close");
	gtk_widget_set_tooltip_text(button, close_accel);
	gtk_window_set_default_widget(GTK_WINDOW(dw->window), button);
	dupe_dnd_init(dw);

	GtkGesture *context_gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_gesture), GDK_BUTTON_SECONDARY);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(context_gesture), GTK_PHASE_CAPTURE);
	g_signal_connect(context_gesture, "pressed", G_CALLBACK(dupe_listview_press_cb), dw);
	gtk_widget_add_controller(dw->listview, GTK_EVENT_CONTROLLER(context_gesture));

	context_gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_gesture), GDK_BUTTON_SECONDARY);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(context_gesture), GTK_PHASE_CAPTURE);
	g_signal_connect(context_gesture, "pressed", G_CALLBACK(dupe_listview_press_cb), dw);
	gtk_widget_add_controller(dw->second_listview, GTK_EVENT_CONTROLLER(context_gesture));

	gtk_window_present(GTK_WINDOW(dw->window));

	dupe_listview_set_height(dw->listview, dw->show_thumbs);
	dupe_menu_type_cb(GTK_DROP_DOWN(dw->drop_down), nullptr, dw);

	dupe_window_update_count(dw, TRUE);
	dupe_window_update_progress(dw, nullptr, 0.0, FALSE);

	dupe_window_list = g_list_append(dupe_window_list, dw);

	file_data_register_notify_func(dupe_notify_cb, dw, NOTIFY_PRIORITY_MEDIUM);

	g_mutex_init(&dw->thread_count_mutex);
	g_mutex_init(&dw->search_matches_mutex);
	dw->dupe_comparison_thread_pool = g_thread_pool_new(dupe_comparison_func, dw, options->threads.duplicates, FALSE, nullptr);

	GApplication *app = g_application_get_default();

	GKeyFile *accels_main_keyfile = get_keyfile_merged();
	register_actions_from_table(GTK_APPLICATION(app), dw->window, dupe_main_actions, accels_main_keyfile, dw);

	GKeyFile *accels_second_keyfile = get_keyfile_merged();
	register_actions_from_table(GTK_APPLICATION(app), dw->window, dupe_second_actions, accels_second_keyfile, dw);

	return dw;
}

/*
 *-------------------------------------------------------------------
 * dnd confirm dir
 *-------------------------------------------------------------------
 */

static void dupe_confirm_dir_list(DupeWindow *dw, GList *list)
{
	auto files = std::shared_ptr<FileDataList>(list, file_data_list_free);
	auto window = std::shared_ptr<GtkWidget>(GTK_WIDGET(g_object_ref(dw->window)), g_object_unref);
	const gboolean second = dw->second_drop;
	folder_contents_dialog(dw->window, _("Find duplicates"), _("Dropped list includes folders"), _("_Add contents"),
		[files, window, second](bool include_folders, bool recursive)
		{
		auto *target = static_cast<DupeWindow *>(g_object_get_data(G_OBJECT(window.get()), DUPE_WINDOW_DATA_KEY));
		if (!target) return;
		target->second_drop = second && target->second_set;
		g_autoptr(FileDataList) selected = nullptr;
		for (GList *work = files.get(); work; work = work->next)
			{
			auto *fd = static_cast<FileData *>(work->data);
			if (include_folders || !isdir(fd->path)) selected = g_list_prepend(selected, file_data_ref(fd));
			}
		selected = g_list_reverse(selected);
		dupe_window_add_files(target, selected, recursive);
		});
}

/*
 *-------------------------------------------------------------------
 * dnd
 *-------------------------------------------------------------------
 */

static void dupe_dnd_select_click_item(DupeWindow *dw, GtkWidget *widget)
{
	if (!dw->click_item || dupe_listview_item_is_selected(dw->click_item, widget)) return;

	const gint position = dupe_listview_find_item(widget, dw->click_item);
	if (position >= 0) dupe_view_select_item(widget, position, true);
}

static GdkContentProvider *dupe_dnd_prepare(GtkDragSource *source, gdouble, gdouble, gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	GtkWidget *cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	GtkWidget *widget = gtk_widget_get_ancestor(cell, GTK_TYPE_COLUMN_VIEW);
	auto *item = static_cast<GtkListItem *>(g_object_get_data(G_OBJECT(cell), "dupe-list-item"));
	if (!widget || !item) return nullptr;
	dw->click_item = dupe_listview_item_at(widget, gtk_list_item_get_position(item));
	if (!dw->click_item) return nullptr;

	dupe_dnd_select_click_item(dw, widget);

	g_autoptr(FileDataList) list = dupe_listview_get_selection(widget);
	if (!list) return nullptr;

	dnd_set_drag_icon(source, dw->click_item->pixbuf, g_list_length(list), dw->click_item->fd);
	return dnd_file_list_content_provider(list);
}

struct DupeDndDropData
{
	GtkWidget *window;
	GtkWidget *widget;
};

static void dupe_dnd_file_received(GdkDrop *drop, GList *list, gpointer data)
{
	g_autofree auto *drop_data = static_cast<DupeDndDropData *>(data);
	auto *dw = static_cast<DupeWindow *>(g_object_get_data(G_OBJECT(drop_data->window), DUPE_WINDOW_DATA_KEY));
	if (!dw)
		{
		gdk_drop_finish(drop, GDK_ACTION_NONE);
		g_object_unref(drop_data->window);
		return;
		}

	auto action = GDK_ACTION_NONE;

	if (dw->add_files_queue_id > 0)
		{
		warning_dialog(_("Find duplicates"), _("Please wait for the current file selection to be loaded."), GQ_ICON_DIALOG_INFO, dw->window);
		}
	else if (list)
		{
		dw->second_drop = (dw->second_set && drop_data->widget == dw->second_listview);

		if (file_data_list_has_dir(list))
			{
			dupe_confirm_dir_list(dw, filelist_copy(list));
			}
		else
			{
			dupe_window_add_files(dw, list, FALSE);
			}
		action = GDK_ACTION_COPY;
		}

	gdk_drop_finish(drop, action);
	g_object_unref(drop_data->window);
}

static gboolean dupe_dnd_drop(GtkDropTargetAsync *target, GdkDrop *drop, gdouble, gdouble, gpointer data)
{
	auto *drop_data = g_new(DupeDndDropData, 1);
	auto *dw = static_cast<DupeWindow *>(data);
	drop_data->window = GTK_WIDGET(g_object_ref(dw->window));
	drop_data->widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));

	dnd_read_file_list_async(drop, dupe_dnd_file_received, drop_data);

	return TRUE;
}

static void dupe_dnd_setup_widget(DupeWindow *dw, GtkWidget *widget)
{
	GdkContentFormats *formats = dnd_file_drop_formats();
	GtkDropTargetAsync *drop_target = gtk_drop_target_async_new(formats, static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_MOVE));
	g_signal_connect(drop_target, "drop", G_CALLBACK(dupe_dnd_drop), dw);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drop_target));
}

static void dupe_dnd_init(DupeWindow *dw)
{
	dupe_dnd_setup_widget(dw, dw->listview);
	dupe_dnd_setup_widget(dw, dw->second_listview);
}

/*
 *-------------------------------------------------------------------
 * maintenance (move, delete, etc.)
 *-------------------------------------------------------------------
 */

static gboolean dupe_deleted_files_update_cb(gpointer data)
{
	auto *dw = static_cast<DupeWindow *>(data);
	// Let file-list loading finish rather than discarding its remaining entries.
	if (dw->add_files_queue_id) return G_SOURCE_CONTINUE;
	dw->delete_update_id = 0;
	g_autoptr(GHashTable) deleted = g_steal_pointer(&dw->pending_deletions);
	g_autoptr(GHashTable) removed = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (GList *list : {dw->list, dw->second_list})
		{
		for (GList *work = list; work; work = work->next)
			{
			auto *item = static_cast<DupeItem *>(work->data);
			if (g_hash_table_contains(deleted, item->fd)) g_hash_table_add(removed, item);
			}
		}
	if (g_hash_table_size(removed) == 0) return G_SOURCE_REMOVE;

	bool restart = dw->idle_id || dw->recompare_id || dw->img_loader;
	if (!restart && dw->second_set && !dw->ignore_same_folder)
		{
		for (GList *group = dw->dupes; group; group = group->next)
			{
			auto *parent = static_cast<DupeItem *>(group->data);
			if (g_hash_table_contains(removed, parent)) restart = true;
			}
		}
	dupe_check_stop(dw);
	g_autoptr(FileDataList) selected = dupe_listview_get_selection(dw->listview);
	g_autoptr(FileDataList) second_selected = dupe_listview_get_selection(dw->second_listview);

	if (!restart && !dw->ignore_same_folder)
		{
		// A completed ordinary group is a star. Reconnect surviving members if
		// its representative was deleted, without recomparing the whole search.
		for (GList *group = dw->dupes; group; group = group->next)
			{
			auto *parent = static_cast<DupeItem *>(group->data);
			if (!g_hash_table_contains(removed, parent)) continue;
			DupeItem *replacement = nullptr;
			for (GList *member = parent->group; member; member = member->next)
				{
				auto *match = static_cast<DupeMatch *>(member->data);
				if (g_hash_table_contains(removed, match->di)) continue;
				if (!replacement) replacement = match->di;
				else if (!dw->second_set)
					{
					gdouble rank = 0;
					if (dupe_match(replacement, match->di, dw->match_mask, &rank, FALSE))
						dupe_match_link(replacement, match->di, rank);
					}
				}
			}
		}

	// Prune all affected edges in one pass before freeing any items.
	for (GList *list : {dw->list, dw->second_list})
		{
		for (GList *work = list; work; work = work->next)
			{
			auto *item = static_cast<DupeItem *>(work->data);
			for (GList *member = item->group; member;)
				{
				GList *next = member->next;
				auto *match = static_cast<DupeMatch *>(member->data);
				if (g_hash_table_contains(removed, match->di))
					{
					g_free(match);
					item->group = g_list_delete_link(item->group, member);
					}
				member = next;
				}
			}
		}
	g_clear_pointer(&dw->dupes, g_list_free);
	g_list_store_remove_all(dupe_view_store(dw->listview));
	auto *second_store = dupe_view_store(dw->second_listview);
	g_list_store_remove_all(second_store);
	for (GList **list : {&dw->list, &dw->second_list})
		{
		for (GList *work = *list; work;)
			{
			GList *next = work->next;
			auto *item = static_cast<DupeItem *>(work->data);
			if (g_hash_table_contains(removed, item))
				{
				if (dw->click_item == item) dw->click_item = nullptr;
				dupe_match_link_clear(item, FALSE);
				*list = g_list_delete_link(*list, work);
				dupe_item_free(item);
				}
			work = next;
			}
		}
	dw->thumb_item = nullptr;
	dw->setup_point = nullptr;
	dw->working = nullptr;
	dw->set_count = 0;
	for (GList *work = dw->second_list; work; work = work->next)
		{
		auto *item = static_cast<DupeItem *>(work->data);
		g_autoptr(DupeRow) row = dupe_row_new(item);
		g_list_store_append(second_store, row);
		}
	dupe_view_sort(dw->second_listview);
	dupe_second_update_status(dw);
	if (restart)
		{
		dupe_match_reset_list(dw->list);
		dupe_match_reset_list(dw->second_list);
		dupe_check_start(dw);
		}
	else
		{
		dupe_match_rank(dw);
		dupe_listview_populate(dw);
		dupe_window_update_count(dw, FALSE);
		if (dw->show_thumbs) dupe_thumb_step(dw);
		}
	const auto restore_selection = [](GtkWidget *view, GList *files)
		{
		g_autoptr(GHashTable) selection = g_hash_table_new(g_direct_hash, g_direct_equal);
		for (GList *work = files; work; work = work->next) g_hash_table_add(selection, work->data);
		auto *rows = dupe_view_selection(view);
		gtk_selection_model_unselect_all(rows);
		const guint count = g_list_model_get_n_items(G_LIST_MODEL(rows));
		for (guint i = 0; i < count; ++i)
			{
			g_autoptr(DupeRow) row = dupe_view_row(view, i);
			if (g_hash_table_contains(selection, row->fd)) gtk_selection_model_select_item(rows, i, FALSE);
			}
		};
	if (!restart) restore_selection(dw->listview, selected);
	restore_selection(dw->second_listview, second_selected);
	return G_SOURCE_REMOVE;
}

static void dupe_notify_cb(FileData *fd, NotifyType type, gpointer data)
{
	auto dw = static_cast<DupeWindow *>(data);

	if (!(type & NOTIFY_CHANGE) || !fd->change) return;

	DEBUG_1("Notify dupe: %s %04x", fd->path, type); // NOTRANSLATE: Diagnostic message.

	switch (fd->change->type)
		{
		case FILEDATA_CHANGE_MOVE:
		case FILEDATA_CHANGE_RENAME:
			dupe_item_update_fd(dw, fd);
			break;
		case FILEDATA_CHANGE_COPY:
			break;
		case FILEDATA_CHANGE_DELETE:
			if (!dw->pending_deletions)
				{
				dw->pending_deletions = g_hash_table_new_full(g_direct_hash, g_direct_equal,
					[](gpointer item) { file_data_unref(static_cast<FileData *>(item)); }, nullptr);
				}
			g_hash_table_add(dw->pending_deletions, file_data_ref(fd));
			if (!dw->delete_update_id) dw->delete_update_id = g_timeout_add(100, dupe_deleted_files_update_cb, dw);
			break;
		case FILEDATA_CHANGE_UNSPECIFIED:
		case FILEDATA_CHANGE_WRITE_METADATA:
			break;
		}

}

const ActionDef *get_dupe_main_actions()
{
return dupe_main_actions;
}

const ActionDef *get_dupe_second_actions()
{
return dupe_second_actions;
}

/* vim: set shiftwidth=8 softtabstop=0 cindent cinoptions={1s: */
