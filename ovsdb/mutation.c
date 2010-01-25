/* Copyright (c) 2009, 2010 Nicira Networks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "mutation.h"

#include <float.h>
#include <limits.h>

#include "column.h"
#include "ovsdb-error.h"
#include "json.h"
#include "row.h"
#include "table.h"

enum mutate_error {
    ME_OK,
    ME_DOM,
    ME_RANGE,
    ME_COUNT,
    ME_DUP
};

struct ovsdb_error *
ovsdb_mutator_from_string(const char *name, enum ovsdb_mutator *mutator)
{
#define OVSDB_MUTATOR(ENUM, NAME)               \
    if (!strcmp(name, NAME)) {                  \
        *mutator = ENUM;                        \
        return NULL;                            \
    }
    OVSDB_MUTATORS;
#undef OVSDB_MUTATOR

    return ovsdb_syntax_error(NULL, "unknown mutator",
                              "No mutator named %s.", name);
}

const char *
ovsdb_mutator_to_string(enum ovsdb_mutator mutator)
{
    switch (mutator) {
#define OVSDB_MUTATOR(ENUM, NAME) case ENUM: return NAME;
        OVSDB_MUTATORS;
#undef OVSDB_MUTATOR
    }

    return NULL;
}

static WARN_UNUSED_RESULT struct ovsdb_error *
type_mismatch(const struct ovsdb_mutation *m, const struct json *json)
{
    struct ovsdb_error *error;
    char *s;

    s = ovsdb_type_to_english(&m->column->type);
    error = ovsdb_syntax_error(
        json, NULL, "Type mismatch: \"%s\" operator may not be "
        "applied to column %s of type %s.",
        ovsdb_mutator_to_string(m->mutator), m->column->name, s);
    free(s);

    return error;
}

static WARN_UNUSED_RESULT struct ovsdb_error *
ovsdb_mutation_from_json(const struct ovsdb_table_schema *ts,
                         const struct json *json,
                         const struct ovsdb_symbol_table *symtab,
                         struct ovsdb_mutation *m)
{
    const struct json_array *array;
    struct ovsdb_error *error;
    const char *mutator_name;
    const char *column_name;

    if (json->type != JSON_ARRAY
        || json->u.array.n != 3
        || json->u.array.elems[0]->type != JSON_STRING
        || json->u.array.elems[1]->type != JSON_STRING) {
        return ovsdb_syntax_error(json, NULL, "Parse error in mutation.");
    }
    array = json_array(json);

    column_name = json_string(array->elems[0]);
    m->column = ovsdb_table_schema_get_column(ts, column_name);
    if (!m->column) {
        return ovsdb_syntax_error(json, "unknown column",
                                  "No column %s in table %s.",
                                  column_name, ts->name);
    }
    m->type = m->column->type;

    mutator_name = json_string(array->elems[1]);
    error = ovsdb_mutator_from_string(mutator_name, &m->mutator);
    if (error) {
        return error;
    }

    /* Type-check and relax restrictions on 'type' if appropriate.  */
    switch (m->mutator) {
    case OVSDB_M_ADD:
    case OVSDB_M_SUB:
    case OVSDB_M_MUL:
    case OVSDB_M_DIV:
    case OVSDB_M_MOD:
        if ((!ovsdb_type_is_scalar(&m->type) && !ovsdb_type_is_set(&m->type))
            || (m->type.key_type != OVSDB_TYPE_INTEGER
                && m->type.key_type != OVSDB_TYPE_REAL)
            || (m->mutator == OVSDB_M_MOD
                && m->type.key_type == OVSDB_TYPE_REAL)) {
            return type_mismatch(m, json);
        }
        m->type.n_min = m->type.n_max = 1;
        return ovsdb_datum_from_json(&m->arg, &m->type, array->elems[2],
                                     symtab);

    case OVSDB_M_INSERT:
    case OVSDB_M_DELETE:
        if (!ovsdb_type_is_set(&m->type) && !ovsdb_type_is_map(&m->type)) {
            return type_mismatch(m, json);
        }
        m->type.n_min = 0;
        if (m->mutator == OVSDB_M_DELETE) {
            m->type.n_max = UINT_MAX;
        }
        error = ovsdb_datum_from_json(&m->arg, &m->type, array->elems[2],
                                      symtab);
        if (error && ovsdb_type_is_map(&m->type)
            && m->mutator == OVSDB_M_DELETE) {
            ovsdb_error_destroy(error);
            m->type.value_type = OVSDB_TYPE_VOID;
            error = ovsdb_datum_from_json(&m->arg, &m->type, array->elems[2],
                                          symtab);
        }
        return error;
    }

    NOT_REACHED();
}

static void
ovsdb_mutation_free(struct ovsdb_mutation *m)
{
    ovsdb_datum_destroy(&m->arg, &m->type);
}

struct ovsdb_error *
ovsdb_mutation_set_from_json(const struct ovsdb_table_schema *ts,
                             const struct json *json,
                             const struct ovsdb_symbol_table *symtab,
                             struct ovsdb_mutation_set *set)
{
    const struct json_array *array = json_array(json);
    size_t i;

    set->mutations = xmalloc(array->n * sizeof *set->mutations);
    set->n_mutations = 0;
    for (i = 0; i < array->n; i++) {
        struct ovsdb_error *error;
        error = ovsdb_mutation_from_json(ts, array->elems[i], symtab,
                                         &set->mutations[i]);
        if (error) {
            ovsdb_mutation_set_destroy(set);
            set->mutations = NULL;
            set->n_mutations = 0;
            return error;
        }
        set->n_mutations++;
    }

    return NULL;
}

static struct json *
ovsdb_mutation_to_json(const struct ovsdb_mutation *m)
{
    return json_array_create_3(
        json_string_create(m->column->name),
        json_string_create(ovsdb_mutator_to_string(m->mutator)),
        ovsdb_datum_to_json(&m->arg, &m->type));
}

struct json *
ovsdb_mutation_set_to_json(const struct ovsdb_mutation_set *set)
{
    struct json **mutations;
    size_t i;

    mutations = xmalloc(set->n_mutations * sizeof *mutations);
    for (i = 0; i < set->n_mutations; i++) {
        mutations[i] = ovsdb_mutation_to_json(&set->mutations[i]);
    }
    return json_array_create(mutations, set->n_mutations);
}

void
ovsdb_mutation_set_destroy(struct ovsdb_mutation_set *set)
{
    size_t i;

    for (i = 0; i < set->n_mutations; i++) {
        ovsdb_mutation_free(&set->mutations[i]);
    }
    free(set->mutations);
}

static int
add_int(int64_t *x, int64_t y)
{
    /* Check for overflow.  See _Hacker's Delight_ pp. 27. */
    int64_t z = ~(*x ^ y) & INT64_MIN;
    if ((~(*x ^ y) & ~(((*x ^ z) + y) ^ y)) >> 63) {
        return ME_RANGE;
    } else {
        *x += y;
        return 0;
    }
}

static int
sub_int(int64_t *x, int64_t y)
{
    /* Check for overflow.  See _Hacker's Delight_ pp. 27. */
    int64_t z = (*x ^ y) & INT64_MIN;
    if (((*x ^ y) & (((*x ^ z) - y) ^ y)) >> 63) {
        return ME_RANGE;
    } else {
        *x -= y;
        return 0;
    }
}

static int
mul_int(int64_t *x, int64_t y)
{
    /* Check for overflow.  See _Hacker's Delight_ pp. 30. */
    if (*x > 0
        ? (y > 0
           ? *x >= INT64_MAX / y
           : y  < INT64_MIN / *x)
        : (y > 0
           ? *x < INT64_MIN / y
           : *x != 0 && y < INT64_MAX / y)) {
        return ME_RANGE;
    } else {
        *x *= y;
        return 0;
    }
}

static int
check_int_div(int64_t x, int64_t y)
{
    /* Check for overflow.  See _Hacker's Delight_ pp. 32. */
    if (!y) {
        return ME_DOM;
    } else if (x == INT64_MIN && y == -1) {
        return ME_RANGE;
    } else {
        return 0;
    }
}

static int
div_int(int64_t *x, int64_t y)
{
    int error = check_int_div(*x, y);
    if (!error) {
        *x /= y;
    }
    return error;
}

static int
mod_int(int64_t *x, int64_t y)
{
    int error = check_int_div(*x, y);
    if (!error) {
        *x %= y;
    }
    return error;
}

static int
check_real_range(double x)
{
    return x >= -DBL_MAX && x <= DBL_MAX ? 0 : ME_RANGE;
}

static int
add_double(double *x, double y)
{
    *x += y;
    return 0;
}

static int
sub_double(double *x, double y)
{
    *x -= y;
    return 0;
}

static int
mul_double(double *x, double y)
{
    *x *= y;
    return 0;
}

static int
div_double(double *x, double y)
{
    if (y == 0) {
        return ME_DOM;
    } else {
        *x /= y;
        return 0;
    }
}

static int
mutate_scalar(const struct ovsdb_type *dst_type, struct ovsdb_datum *dst,
              const union ovsdb_atom *arg,
              int (*mutate_integer)(int64_t *x, int64_t y),
              int (*mutate_real)(double *x, double y))
{
    struct ovsdb_error *error;
    unsigned int i;

    if (dst_type->key_type == OVSDB_TYPE_INTEGER) {
        int64_t y = arg->integer;
        for (i = 0; i < dst->n; i++) {
            int error = mutate_integer(&dst->keys[i].integer, y);
            if (error) {
                return error;
            }
        }
    } else if (dst_type->key_type == OVSDB_TYPE_REAL) {
        double y = arg->real;
        for (i = 0; i < dst->n; i++) {
            double *x = &dst->keys[i].real;
            int error = mutate_real(x, y);
            if (!error) {
                error = check_real_range(*x);
            }
            if (error) {
                return error;
            }
        }
    } else {
        NOT_REACHED();
    }

    error = ovsdb_datum_sort(dst, dst_type);
    if (error) {
        ovsdb_error_destroy(error);
        return ME_DUP;
    }
    return 0;
}

struct ovsdb_error *
ovsdb_mutation_set_execute(struct ovsdb_row *row,
                           const struct ovsdb_mutation_set *set)
{
    size_t i;

    for (i = 0; i < set->n_mutations; i++) {
        const struct ovsdb_mutation *m = &set->mutations[i];
        struct ovsdb_datum *dst = &row->fields[m->column->index];
        const struct ovsdb_type *dst_type = &m->column->type;
        const struct ovsdb_datum *arg = &set->mutations[i].arg;
        const struct ovsdb_type *arg_type = &m->type;
        int error;

        switch (m->mutator) {
        case OVSDB_M_ADD:
            error = mutate_scalar(dst_type, dst, &arg->keys[0],
                                  add_int, add_double);
            break;

        case OVSDB_M_SUB:
            error = mutate_scalar(dst_type, dst, &arg->keys[0],
                                  sub_int, sub_double);
            break;

        case OVSDB_M_MUL:
            error = mutate_scalar(dst_type, dst, &arg->keys[0],
                                  mul_int, mul_double);
            break;

        case OVSDB_M_DIV:
            error = mutate_scalar(dst_type, dst, &arg->keys[0],
                                  div_int, div_double);
            break;

        case OVSDB_M_MOD:
            error = mutate_scalar(dst_type, dst, &arg->keys[0],
                                  mod_int, NULL);
            break;

        case OVSDB_M_INSERT:
            ovsdb_datum_union(dst, arg, dst_type, false);
            error = ovsdb_datum_conforms_to_type(dst, dst_type) ? 0 : ME_COUNT;
            break;

        case OVSDB_M_DELETE:
            ovsdb_datum_subtract(dst, dst_type, arg, arg_type);
            error = ovsdb_datum_conforms_to_type(dst, dst_type) ? 0 : ME_COUNT;
            break;
        }

        switch (error) {
        case 0:
            break;

        case ME_DOM:
            return ovsdb_error("domain error", "Division by zero.");

        case ME_RANGE:
            return ovsdb_error("range error",
                               "Result of \"%s\" operation is out of range.",
                               ovsdb_mutator_to_string(m->mutator));

        case ME_DUP:
            return ovsdb_error("constraint violation",
                               "Result of \"%s\" operation contains "
                               "duplicates.",
                               ovsdb_mutator_to_string(m->mutator));

        case ME_COUNT: {
            char *s = ovsdb_type_to_english(dst_type);
            struct ovsdb_error *e = ovsdb_error(
                "constaint violation",
                "Attempted to store %u elements in %s.", dst->n, s);
            free(s);
            return e;
        }

        default:
            return OVSDB_BUG("unexpected errno");
        }
    }

    return NULL;
}
