#include "hb-vault-page.h"
#include "hb-icons.h"
#include "hb-hosts-view.h"
#include "hb-keychain-view.h"
#include "hb-settings-view.h"
#include "hb-snippets-view.h"
#include "hb-known-view.h"
#include "hb-logs-view.h"
#include "hb-forward-view.h"
#include "hb-store.h"

#include <stdio.h>

/* 与 prototype SECTIONS 顺序/标签一致；icon 为 P 表键，"{ }" 走 mono glyph */
static const struct { const char *id, *label, *icon, *fr; } SECTIONS[] = {
    { "hosts",       "Hosts",          "grid",      NULL     },
    { "keys",        "Keychain",       "key",       "FR-04"  },
    { "fwd",         "Port Forwarding","fwd",       "FR-11"  },
    { "snippets",    "Snippets",       "{ }",       "FR-17"  },
    { "known_hosts", "Known Hosts",    "shield",    "FR-05"  },
    { "logs",        "Logs",           "clock",     "FR-16"  },
    { "share",       "共享会话",        "broadcast", "FR-14"  },
    { "appearance",  "外观与终端",      "palette",   "FR-13"  },
    { "account",     "账号与同步",      "cloud",     "FR-19"  },
};
#define N_SECTIONS (sizeof(SECTIONS) / sizeof(SECTIONS[0]))

struct _HbVaultPage {
    GtkBox parent;
    GtkWidget *sidebar;
    GtkWidget *stack;
    GtkWidget *hosts_view;
    GtkWidget *side_btns[N_SECTIONS];
    const char *active;
};

G_DEFINE_TYPE(HbVaultPage, hb_vault_page, GTK_TYPE_BOX)

static GtkWidget *empty_page(const char *id, const char *label,
                             const char *icon, const char *fr)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *ico = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *glyph;
    GtkWidget *t, *s;
    char sub[128];

    gtk_widget_add_css_class(box, "hb-empty");
    gtk_widget_add_css_class(box, "hb-content");
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(box, TRUE);

    gtk_widget_add_css_class(ico, "hb-e-ico");
    gtk_widget_set_size_request(ico, 58, 58);
    gtk_widget_set_halign(ico, GTK_ALIGN_CENTER);
    if (!strcmp(icon, "{ }")) {
        glyph = hb_mono_icon_new("{ }");
    } else {
        glyph = hb_icon_new(icon, 24);
    }
    gtk_widget_set_halign(glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(glyph, TRUE);
    gtk_box_append(GTK_BOX(ico), glyph);
    gtk_box_append(GTK_BOX(box), ico);

    t = gtk_label_new(label);
    gtk_widget_add_css_class(t, "hb-e-title");
    gtk_box_append(GTK_BOX(box), t);

    snprintf(sub, sizeof sub, "%s · 该功能将在后续迭代中实现", fr);
    s = gtk_label_new(sub);
    gtk_widget_add_css_class(s, "hb-e-sub");
    gtk_box_append(GTK_BOX(box), s);

    gtk_widget_set_name(box, id);
    return box;
}

static void on_section_click(GtkButton *b, gpointer ud)
{
    HbVaultPage *vp = ud;
    const char *id = g_object_get_data(G_OBJECT(b), "sec");
    hb_vault_page_set_section(vp, id);
}

void hb_vault_page_set_section(HbVaultPage *vp, const char *section)
{
    size_t i;
    for (i = 0; i < N_SECTIONS; i++) {
        gboolean on = !strcmp(SECTIONS[i].id, section);
        if (on) {
            gtk_widget_add_css_class(vp->side_btns[i], "active");
            gtk_stack_set_visible_child_name(GTK_STACK(vp->stack),
                                           SECTIONS[i].id);
            vp->active = SECTIONS[i].id;
        } else {
            gtk_widget_remove_css_class(vp->side_btns[i], "active");
        }
    }
}

void hb_vault_page_toggle_sidebar(HbVaultPage *vp)
{
    gtk_widget_set_visible(vp->sidebar,
                           !gtk_widget_get_visible(vp->sidebar));
}

GtkWidget *hb_vault_page_hosts_view(HbVaultPage *vp)
{
    return vp->hosts_view;
}

static GtkWidget *build_sidebar(HbVaultPage *vp)
{
    GtkWidget *sb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    size_t i;
    GtkWidget *sep, *chip, *av, *vb, *n, *s;

    gtk_widget_add_css_class(sb, "hb-sidebar");
    for (i = 0; i < N_SECTIONS; i++) {
        GtkWidget *btn = gtk_button_new();
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *ic;
        gtk_widget_add_css_class(btn, "hb-side-item");
        gtk_widget_set_hexpand(btn, TRUE);
        if (!strcmp(SECTIONS[i].icon, "{ }"))
            ic = hb_mono_icon_new("{ }");
        else
            ic = hb_icon_new(SECTIONS[i].icon, 17);
        gtk_widget_add_css_class(ic, "s-ico");
        gtk_widget_set_halign(ic, GTK_ALIGN_CENTER);
        {
            GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_size_request(slot, 18, -1);
            gtk_widget_set_hexpand_set(slot, TRUE);
            gtk_widget_set_hexpand(slot, FALSE);
            gtk_box_append(GTK_BOX(slot), ic);
            gtk_box_append(GTK_BOX(row), slot);
        }
        gtk_box_append(GTK_BOX(row), gtk_label_new(SECTIONS[i].label));
        gtk_button_set_child(GTK_BUTTON(btn), row);
        g_object_set_data(G_OBJECT(btn), "sec", (gpointer)SECTIONS[i].id);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_section_click), vp);
        gtk_box_append(GTK_BOX(sb), btn);
        vp->side_btns[i] = btn;
    }

    gtk_widget_set_vexpand(sep = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0),
                         TRUE);   /* 占位推开底栏 */
    gtk_box_append(GTK_BOX(sb), sep);
    sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sep, "hb-side-sep");
    gtk_widget_set_size_request(sep, -1, 1);
    gtk_box_append(GTK_BOX(sb), sep);

    /* 账号芯片：无账号系统 → 未登录态 */
    chip = gtk_button_new();
    gtk_widget_add_css_class(chip, "hb-account-chip");
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        char initial[8] = "?";
        const char *un = g_get_user_name();
        av = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(av, "hb-avatar");
        gtk_widget_set_size_request(av, 26, 26);
        gtk_widget_set_hexpand_set(av, TRUE);
        gtk_widget_set_hexpand(av, FALSE);
        if (un && *un)
            g_snprintf(initial, sizeof initial, "%c",
                       g_ascii_toupper(*un));
        {
            GtkWidget *l = gtk_label_new(initial);
            gtk_widget_set_halign(l, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
            gtk_widget_set_hexpand(l, TRUE);
            gtk_box_append(GTK_BOX(av), l);
        }
        gtk_box_append(GTK_BOX(row), av);
        vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        n = gtk_label_new("未登录");
        gtk_widget_add_css_class(n, "hb-a-name");
        gtk_label_set_xalign(GTK_LABEL(n), 0);
        gtk_box_append(GTK_BOX(vb), n);
        s = gtk_label_new("点击登录");
        gtk_widget_add_css_class(s, "hb-a-sub");
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_box_append(GTK_BOX(vb), s);
        gtk_box_append(GTK_BOX(row), vb);
        gtk_button_set_child(GTK_BUTTON(chip), row);
    }
    gtk_box_append(GTK_BOX(sb), chip);
    return sb;
}

static void hb_vault_page_init(HbVaultPage *vp)
{
    size_t i;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(vp),
                                   GTK_ORIENTATION_HORIZONTAL);
    gtk_box_set_spacing(GTK_BOX(vp), 0);

    vp->sidebar = build_sidebar(vp);
    gtk_box_append(GTK_BOX(vp), vp->sidebar);

    vp->stack = gtk_stack_new();
    gtk_widget_set_hexpand(vp->stack, TRUE);
    gtk_widget_add_css_class(vp->stack, "hb-content");

    vp->hosts_view = hb_hosts_view_new(hb_store_default());
    gtk_stack_add_named(GTK_STACK(vp->stack), vp->hosts_view, "hosts");
    gtk_stack_add_named(GTK_STACK(vp->stack),
        hb_keychain_view_new(hb_store_default()), "keys");
    for (i = 2; i < N_SECTIONS; i++) {
        GtkWidget *page;
        if (!strcmp(SECTIONS[i].id, "appearance"))
            page = hb_settings_view_new();
        else if (!strcmp(SECTIONS[i].id, "snippets"))
            page = hb_snippets_view_new(hb_store_default());
        else if (!strcmp(SECTIONS[i].id, "known_hosts"))
            page = hb_known_view_new(hb_store_default());
        else if (!strcmp(SECTIONS[i].id, "logs"))
            page = hb_logs_view_new(hb_store_default());
        else if (!strcmp(SECTIONS[i].id, "fwd"))
            page = hb_forward_view_new(hb_store_default());
        else
            page = empty_page(SECTIONS[i].id, SECTIONS[i].label,
                              SECTIONS[i].icon, SECTIONS[i].fr);
        gtk_stack_add_named(GTK_STACK(vp->stack), page,
                            SECTIONS[i].id);
    }
    gtk_box_append(GTK_BOX(vp), vp->stack);
    hb_vault_page_set_section(vp, "hosts");
}

static void hb_vault_page_class_init(HbVaultPageClass *klass)
{ (void)klass; }

GtkWidget *hb_vault_page_new(void)
{
    return g_object_new(HB_TYPE_VAULT_PAGE, NULL);
}
