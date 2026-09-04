/* Not-yet-translated routines.
 *
 * Nothing left: every routine the main line reaches is translated in
 * its module (see CONVENTIONS.md for the map).  The file stays so
 * build_mod.bat's staging builds keep working; a module that grows a
 * new cross-module dependency puts its stub here, under
 * #ifndef AD_HAVE_<module>, until the callee exists.
 */
#include "astdelux.h"
