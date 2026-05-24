#include "game_config.h"

namespace minish_cap {

std::string_view default_region() {
    // The build can override this with -DMINISHCAP_DEFAULT_REGION=...
#ifdef MINISHCAP_DEFAULT_REGION
    return MINISHCAP_DEFAULT_REGION;
#else
    return "usa";
#endif
}

}  // namespace minish_cap
