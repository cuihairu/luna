/* Test shim: a C++ view of replxx's opaque candidate list.
 *
 * replxx exposes replxx_completions only as an incomplete C type — the
 * struct that carries the collected candidates is defined inside
 * replxx.cxx, and the C API offers no constructor, no destructor and no
 * way to read the list back. The line-editor tests need all three: they
 * hand the bridge a candidate list and then check what it collected.
 * This translation unit is the only place that can do it, so the struct
 * definition is repeated here verbatim (identical to replxx.cxx, hence
 * the same class by the ODR) and a handful of C entry points are
 * exported for the C test to drive.
 */
#include <cstddef>

#include <replxx.hxx>

struct replxx_completions {
    replxx::Replxx::completions_t data;
};

extern "C" {

replxx_completions *luna_test_completions_new(void)
{
    return new replxx_completions();
}

void luna_test_completions_free(replxx_completions *c)
{
    delete c;
}

size_t luna_test_completions_count(const replxx_completions *c)
{
    return c->data.size();
}

const char *luna_test_completions_at(const replxx_completions *c, size_t i)
{
    return c->data[i].text().c_str();
}

} /* extern "C" */
