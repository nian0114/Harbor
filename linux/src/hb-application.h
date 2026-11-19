#ifndef HB_APPLICATION_H
#define HB_APPLICATION_H

#include <adwaita.h>

G_BEGIN_DECLS

#define HB_TYPE_APPLICATION (hb_application_get_type())
G_DECLARE_FINAL_TYPE(HbApplication, hb_application, HB, APPLICATION, AdwApplication)

HbApplication *hb_application_new(void);

G_END_DECLS

#endif
