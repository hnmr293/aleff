#include "json.h"

#include "json_decoder.h"
#include "json_encoder.h"

int
adapter_json_install(PyObject *module)
{
    if (adapter_json_encoder_install(module) < 0) {
        return -1;
    }
    if (adapter_json_decoder_install(module) < 0) {
        adapter_json_encoder_rollback();
        return -1;
    }
    return 0;
}

void
adapter_json_rollback(void)
{
    adapter_json_decoder_rollback();
    adapter_json_encoder_rollback();
}
