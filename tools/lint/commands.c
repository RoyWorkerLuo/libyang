/**
 * @file commands.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief libyang's yanglint tool commands
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>

#include "commands.h"
#include "../../src/libyang.h"
#include "../../src/tree_schema.h"
#include "../../src/tree_data.h"
#include "../../src/parser.h"
#include "../../src/xpath.h"

COMMAND commands[];
extern int done;
extern struct ly_ctx *ctx;
extern char *search_path;

void
cmd_add_help(void)
{
    printf("add <path-to-model> [<other-models> ...]\n");
}

void
cmd_print_help(void)
{
    printf("print [-f (yang | tree | info)] [-t <info-target-node>] [-o <output-file>] <model-name>[@<revision>]\n\n");
    printf("\tinfo-target-node: <absolute-schema-node> | typedef/<typedef-name> |\n");
    printf("\t                  | identity/<identity-name> | feature/<feature-name> |\n");
    printf("\t                  | grouping/<grouping-name>(<absolute-schema-nodeid>) |\n");
    printf("\t                  | type/<absolute-schema-node-leaf-or-leaflist>\n");
    printf("\n");
    printf("\tabsolute-schema-nodeid: ( /(<import-prefix>:)<node-identifier> )+\n");
}

static void
cmd_parse_help(const char *name)
{
    printf("%s [-f (xml | json)] [-o <output-file>] <%s-file-name>\n", name, name);
}

void
cmd_data_help(void)
{
    cmd_parse_help("data");
}

void
cmd_config_help(void)
{
    cmd_parse_help("config");
}

void
cmd_filter_help(void)
{
    cmd_parse_help("filter");
}

void
cmd_xpath_help(void)
{
    printf("xpath -e <XPath-expression> [-c <context-node-path>] <XML-data-file-name>\n\n");
    printf("\tcontext-node-path: /<node-name>(/<node-name>)*\n\n");
    printf("\tIf context node is not specififed, data root is used.\n");
    printf("\tIf context node is explicitly specified, \"when\" and \"must\"\n");
    printf("\tdata tree access restrictions are applied.\n");
}

void
cmd_list_help(void)
{
    printf("list\n");
}

void
cmd_feature_help(void)
{
    printf("feature [ -(-e)nable | -(-d)isable (* | <feature-name>[,<feature-name> ...]) ] <model-name>[@<revision>]\n");
}

void
cmd_searchpath_help(void)
{
    printf("searchpath <model-dir-path>\n");
}

void
cmd_verb_help(void)
{
    printf("verb (error/0 | warning/1 | verbose/2 | debug/3)\n");
}

int
cmd_add(const char *arg)
{
    int fd, path_len;
    char *addr, *ptr, *path;
    const char *arg_ptr;
    struct lys_module *model;
    struct stat sb;
    LYS_INFORMAT format;

    if (strlen(arg) < 5) {
        cmd_add_help();
        return 1;
    }

    arg_ptr = arg + strlen("add ");
    while (arg_ptr[0] == ' ') {
        ++arg_ptr;
    }
    if (strchr(arg_ptr, ' ')) {
        path_len = strchr(arg_ptr, ' ') - arg_ptr;
    } else {
        path_len = strlen(arg_ptr);
    }

    path = strndup(arg_ptr, path_len);

    while (path) {
        if ((ptr = strrchr(path, '.')) != NULL) {
            ++ptr;
            if (!strcmp(ptr, "yin")) {
                format = LYS_IN_YIN;
            } else if (!strcmp(ptr, "yang")) {
                format = LYS_IN_YANG;
            } else {
                fprintf(stderr, "Input file in an unknown format \"%s\".\n", ptr);
                free(path);
                return 1;
            }
        } else {
            fprintf(stdout, "Input file \"%.*s\" without extension, assuming YIN format.\n", path_len, arg_ptr);
            format = LYS_IN_YIN;
        }

        fd = open(path, O_RDONLY);
        free(path);
        if (fd == -1) {
            fprintf(stderr, "Opening input file \"%.*s\" failed (%s).\n", path_len, arg_ptr, strerror(errno));
            return 1;
        }

        if (fstat(fd, &sb) == -1) {
            fprintf(stderr, "Unable to get input file \"%.*s\" information (%s).\n", path_len, arg_ptr, strerror(errno));
            close(fd);
            return 1;
        }

        if (!S_ISREG(sb.st_mode)) {
            fprintf(stderr, "Input file \"%.*s\" not a file.\n", path_len, arg_ptr);
            close(fd);
            return 1;
        }

        addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

        model = lys_parse(ctx, addr, format);
        munmap(addr, sb.st_size);
        close(fd);

        if (!model) {
            /* libyang printed the error messages */
            return 1;
        }

        /* next model */
        arg_ptr += path_len;
        while (arg_ptr[0] == ' ') {
            ++arg_ptr;
        }
        if (strchr(arg_ptr, ' ')) {
            path_len = strchr(arg_ptr, ' ') - arg_ptr;
        } else {
            path_len = strlen(arg_ptr);
        }

        if (path_len) {
            path = strndup(arg_ptr, path_len);
        } else {
            path = NULL;
        }
    }

    return 0;
}

int
cmd_print(const char *arg)
{
    int c, i, argc, option_index, ret = 1;
    char **argv = NULL, *ptr, *target_node = NULL, *model_name, *revision;
    const char **names, *out_path = NULL;
    struct lys_module *model, *parent_model;
    LYS_OUTFORMAT format = LYS_OUT_TREE;
    FILE *output = stdout;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"format", required_argument, 0, 'f'},
        {"output", required_argument, 0, 'o'},
        {"target-node", required_argument, 0, 't'},
        {NULL, 0, 0, 0}
    };

    argc = 1;
    argv = malloc(2*sizeof *argv);
    *argv = strdup(arg);
    ptr = strtok(*argv, " ");
    while ((ptr = strtok(NULL, " "))) {
        argv = realloc(argv, (argc+2)*sizeof *argv);
        argv[argc++] = ptr;
    }
    argv[argc] = NULL;

    optind = 0;
    while (1) {
        option_index = 0;
        c = getopt_long(argc, argv, "hf:o:t:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            cmd_print_help();
            ret = 0;
            goto cleanup;
        case 'f':
            if (!strcmp(optarg, "yang")) {
                format = LYS_OUT_YANG;
            } else if (!strcmp(optarg, "tree")) {
                format = LYS_OUT_TREE;
            } else if (!strcmp(optarg, "info")) {
                format = LYS_OUT_INFO;
            } else {
                fprintf(stderr, "Unknown output format \"%s\".\n", optarg);
                goto cleanup;
            }
            break;
        case 'o':
            if (out_path) {
                fprintf(stderr, "Output specified twice.\n");
                goto cleanup;
            }
            out_path = optarg;
            break;
        case 't':
            target_node = optarg;
            break;
        case '?':
            fprintf(stderr, "Unknown option \"%d\".\n", (char)c);
            goto cleanup;
        }
    }

    /* file name */
    if (optind == argc) {
        fprintf(stderr, "Missing the model name.\n");
        goto cleanup;
    }

    /* model, revision */
    model_name = argv[optind];
    revision = NULL;
    if (strchr(model_name, '@')) {
        revision = strchr(model_name, '@');
        revision[0] = '\0';
        ++revision;
    }

    model = ly_ctx_get_module(ctx, model_name, revision);
    if (model == NULL) {
        names = ly_ctx_get_module_names(ctx);
        for (i = 0; names[i]; i++) {
            if (!model) {
                parent_model = ly_ctx_get_module(ctx, names[i], NULL);
                model = (struct lys_module *)ly_ctx_get_submodule(parent_model, model_name, revision);
            }
        }
        free(names);
    }

    if (model == NULL) {
        if (revision) {
            fprintf(stderr, "No model \"%s\" in revision %s found.\n", model_name, revision);
        } else {
            fprintf(stderr, "No model \"%s\" found.\n", model_name);
        }
        goto cleanup;
    }

    if (out_path) {
        output = fopen(out_path, "w");
        if (!output) {
            fprintf(stderr, "Could not open the output file (%s).\n", strerror(errno));
            goto cleanup;
        }
    }

    ret = lys_print(output, model, format, target_node);

cleanup:
    free(*argv);
    free(argv);

    if (output && (output != stdout)) {
        fclose(output);
    }

    return ret;
}

static int
cmd_parse(const char *arg, int options)
{
    int c, argc, option_index, ret = 1, fd = -1;
    struct stat sb;
    char **argv = NULL, *ptr, *addr;
    const char *out_path = NULL;
    struct lyd_node *data = NULL, *next, *iter;
    LYD_FORMAT format = LYD_UNKNOWN;
    FILE *output = stdout;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"format", required_argument, 0, 'f'},
        {"output", required_argument, 0, 'o'},
        {"strict", no_argument, 0, 's'},
        {NULL, 0, 0, 0}
    };

    argc = 1;
    argv = malloc(2*sizeof *argv);
    *argv = strdup(arg);
    ptr = strtok(*argv, " ");
    while ((ptr = strtok(NULL, " "))) {
        argv = realloc(argv, (argc+2)*sizeof *argv);
        argv[argc++] = ptr;
    }
    argv[argc] = NULL;

    optind = 0;
    while (1) {
        option_index = 0;
        c = getopt_long(argc, argv, "hf:o:xyz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            cmd_data_help();
            ret = 0;
            goto cleanup;
        case 'f':
            if (!strcmp(optarg, "xml")) {
                format = LYD_XML_FORMAT;
            } else if (!strcmp(optarg, "json")) {
                format = LYD_JSON;
            } else {
                fprintf(stderr, "Unknown output format \"%s\".\n", optarg);
                goto cleanup;
            }
            break;
        case 'o':
            if (out_path) {
                fprintf(stderr, "Output specified twice.\n");
                goto cleanup;
            }
            out_path = optarg;
            break;
        case 's':
            options |= LYD_OPT_STRICT;
            break;
        case '?':
            fprintf(stderr, "Unknown option \"%d\".\n", (char)c);
            goto cleanup;
        }
    }

    /* file name */
    if (optind == argc) {
        fprintf(stderr, "Missing the data file name.\n");
        goto cleanup;
    }

    fd = open(argv[optind], O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "The input file could not be opened (%s).\n", strerror(errno));
        goto cleanup;
    }

    if (fstat(fd, &sb) == -1) {
        fprintf(stderr, "Unable to get input file information (%s).\n", strerror(errno));
        goto cleanup;
    }

    if (!S_ISREG(sb.st_mode)) {
        fprintf(stderr, "Input file not a file.\n");
        goto cleanup;
    }

    addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    data = lyd_parse(ctx, addr, LYD_XML, options);
    munmap(addr, sb.st_size);

    if (data == NULL) {
        fprintf(stderr, "Failed to parse data.\n");
        goto cleanup;
    }

    if (out_path) {
        output = fopen(out_path, "w");
        if (!output) {
            fprintf(stderr, "Could not open the output file (%s).\n", strerror(errno));
            goto cleanup;
        }

        if (format == LYD_UNKNOWN) {
            /* default */
            format = LYD_XML;
        }
    }

    if (format != LYD_UNKNOWN) {
        lyd_print(output, data, format);
    }

    ret = 0;

cleanup:
    free(*argv);
    free(argv);

    if (output && (output != stdout)) {
        fclose(output);
    }

    if (fd != -1) {
        close(fd);
    }

    LY_TREE_FOR_SAFE(data, next, iter) {
        lyd_free(iter);
    }

    return ret;
}

int
cmd_data(const char *arg)
{
    return cmd_parse(arg, 0);
}

int
cmd_config(const char *arg)
{
    return cmd_parse(arg, LYD_OPT_EDIT);
}

int
cmd_filter(const char *arg)
{
    return cmd_parse(arg, LYD_OPT_FILTER);
}

int
cmd_xpath(const char *arg)
{
    int c, argc, option_index, fd = -1, ret = 1, long_str, when_must_eval = 0;
    char **argv = NULL, *ptr, *ctx_node_path = NULL, *expr = NULL, *addr;
    struct stat sb;
    struct lyd_node *ctx_node, *data = NULL, *next, *iter;
    struct lyxp_set set = {0, {NULL}, NULL, 0, 0, 0};
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"expr", required_argument, 0, 'e'},
        {"ctx-node", required_argument, 0, 'c'},
        {NULL, 0, 0, 0}
    };

    long_str = 0;
    argc = 1;
    argv = malloc(2 * sizeof *argv);
    *argv = strdup(arg);
    ptr = strtok(*argv, " ");
    while ((ptr = strtok(NULL, " "))) {
        if (long_str) {
            ptr[-1] = ' ';
            if (ptr[strlen(ptr) - 1] == long_str) {
                long_str = 0;
                ptr[strlen(ptr) - 1] = '\0';
            }
        } else {
            argv = realloc(argv, (argc + 2) * sizeof *argv);
            argv[argc] = ptr;
            if (ptr[0] == '"') {
                long_str = '"';
                ++argv[argc];
            }
            if (ptr[0] == '\'') {
                long_str = '\'';
                ++argv[argc];
            }
            if (ptr[strlen(ptr) - 1] == long_str) {
                long_str = 0;
                ptr[strlen(ptr) - 1] = '\0';
            }
            ++argc;
        }
    }
    argv[argc] = NULL;

    optind = 0;
    while (1) {
        option_index = 0;
        c = getopt_long(argc, argv, "he:c:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            cmd_xpath_help();
            ret = 0;
            goto cleanup;
        case 'e':
            expr = optarg;
            break;
        case 'c':
            ctx_node_path = optarg;
            if ((ctx_node_path[0] != '/') || (strlen(ctx_node_path) < 2)
                    || (ctx_node_path[strlen(ctx_node_path) - 1] == '/')) {
                fprintf(stderr, "Invalid context node path \"%s\".\n", ctx_node_path);
                goto cleanup;
            }
            when_must_eval = 1;
            break;
        case '?':
            fprintf(stderr, "Unknown option \"%d\".\n", (char)c);
            goto cleanup;
        }
    }

    if (optind == argc) {
        fprintf(stderr, "Missing the file with data.\n");
        goto cleanup;
    }

    if (!expr) {
        fprintf(stderr, "Missing the XPath expression.\n");
        goto cleanup;
    }

    /* data file */
    fd = open(argv[optind], O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "The input file could not be opened (%s).\n", strerror(errno));
        goto cleanup;
    }
    if (fstat(fd, &sb) == -1) {
        fprintf(stderr, "Unable to get input file information (%s).\n", strerror(errno));
        goto cleanup;
    }
    if (!S_ISREG(sb.st_mode)) {
        fprintf(stderr, "Input file not a file.\n");
        goto cleanup;
    }
    addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    data = lyd_parse(ctx, addr, LYD_XML, 0);
    munmap(addr, sb.st_size);

    if (data == NULL) {
        fprintf(stderr, "Failed to parse data.\n");
        goto cleanup;
    }

    if (ctx_node_path) {
        ctx_node = data;
        ptr = strtok(ctx_node_path, "/");
        do {
            LY_TREE_FOR(ctx_node, ctx_node) {
                if (!strcmp(ctx_node->schema->name, ptr)) {
                    break;
                }
            }
            if (!ctx_node) {
                break;
            }

            ptr = strtok(NULL, "/");
            if (ptr) {
                ctx_node = ctx_node->child;
            }
        } while (ptr && ctx_node);

        if (!ctx_node) {
            fprintf(stderr, "Context node search failed at \"%s\".\n", ptr);
            goto cleanup;
        }
    } else {
        ctx_node = data;
    }

    if (lyxp_eval(expr, ctx_node, &set, when_must_eval, 0)) {
        fprintf(stderr, "XPath expression invalid.\n");
        goto cleanup;
    }

    lyxp_set_print_xml(stdout, &set);
    ret = 0;

cleanup:
    free(*argv);
    free(argv);

    if (fd != -1) {
        close(fd);
    }

    LY_TREE_FOR_SAFE(data, next, iter) {
        lyd_free(iter);
    }

    return ret;
}

int
cmd_list(const char *UNUSED(arg))
{
    struct lyd_node *ylib, *module, *submodule, *node;
    int has_modules = 0;

    ylib = ly_ctx_info(ctx);
    if (!ylib) {
        return 1;
    }

    LY_TREE_FOR(ylib->child, node) {
        if (!strcmp(node->schema->name, "module-set-id")) {
            printf("List of the loaded models (mod-set-id %s):\n", ((struct lyd_node_leaf_list *)node)->value_str);
            break;
        }
    }
    assert(node);

    LY_TREE_FOR(ylib->child, module) {
        if (!strcmp(module->schema->name, "module")) {
            has_modules = 1;

            /* module print */
            LY_TREE_FOR(module->child, node) {
                if (!strcmp(node->schema->name, "name")) {
                    printf("\t%s", ((struct lyd_node_leaf_list *)node)->value_str);
                } else if (!strcmp(node->schema->name, "revision")) {
                    if (((struct lyd_node_leaf_list *)node)->value_str[0] == '\0') {
                        printf("\n");
                    } else {
                        printf("@%s\n", ((struct lyd_node_leaf_list *)node)->value_str);
                    }
                }
            }

            /* submodules print */
            LY_TREE_FOR(module->child, submodule) {
                if (!strcmp(submodule->schema->name, "submodules")) {
                    LY_TREE_FOR(submodule->child, submodule) {
                        if (!strcmp(submodule->schema->name, "submodule")) {
                            LY_TREE_FOR(submodule->child, node) {
                                if (!strcmp(node->schema->name, "name")) {
                                    printf("\t\t%s", ((struct lyd_node_leaf_list *)node)->value_str);
                                } else if (!strcmp(node->schema->name, "revision")) {
                                    if (((struct lyd_node_leaf_list *)node)->value_str[0] == '\0') {
                                        printf("\n");
                                    } else {
                                        printf("@%s\n", ((struct lyd_node_leaf_list *)node)->value_str);
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    if (!has_modules) {
        printf("\t(none)\n");
    }

    lyd_free(ylib);
    return 0;
}

int
cmd_feature(const char *arg)
{
    int c, i, argc, option_index, ret = 1, task = 0;
    unsigned int max_len;
    char **argv = NULL, *ptr, *model_name, *revision, *feat_names = NULL;
    const char **names;
    uint8_t *states;
    struct lys_module *model, *parent_model;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"enable", required_argument, 0, 'e'},
        {"disable", required_argument, 0, 'd'},
        {NULL, 0, 0, 0}
    };

    argc = 1;
    argv = malloc(2*sizeof *argv);
    *argv = strdup(arg);
    ptr = strtok(*argv, " ");
    while ((ptr = strtok(NULL, " "))) {
        argv = realloc(argv, (argc+2)*sizeof *argv);
        argv[argc++] = ptr;
    }
    argv[argc] = NULL;

    optind = 0;
    while (1) {
        option_index = 0;
        c = getopt_long(argc, argv, "he:d:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            cmd_feature_help();
            ret = 0;
            goto cleanup;
        case 'e':
            if (task) {
                fprintf(stderr, "Only one of enable or disable can be specified.\n");
                goto cleanup;
            }
            task = 1;
            feat_names = optarg;
            break;
        case 'd':
            if (task) {
                fprintf(stderr, "Only one of enable, or disable can be specified.\n");
                goto cleanup;
            }
            task = 2;
            feat_names = optarg;
            break;
        case '?':
            fprintf(stderr, "Unknown option \"%d\".\n", (char)c);
            goto cleanup;
        }
    }

    /* model name */
    if (optind == argc) {
        fprintf(stderr, "Missing the model name.\n");
        goto cleanup;
    }

    revision = NULL;
    model_name = argv[optind];
    if (strchr(model_name, '@')) {
        revision = strchr(model_name, '@');
        revision[0] = '\0';
        ++revision;
    }

    model = ly_ctx_get_module(ctx, model_name, revision);
    if (model == NULL) {
        names = ly_ctx_get_module_names(ctx);
        for (i = 0; names[i]; i++) {
            if (!model) {
                parent_model = ly_ctx_get_module(ctx, names[i], NULL);
                model = (struct lys_module *)ly_ctx_get_submodule(parent_model, model_name, revision);
            }
        }
        free(names);
    }
    if (model == NULL) {
        if (revision) {
            fprintf(stderr, "No model \"%s\" in revision %s found.\n", model_name, revision);
        } else {
            fprintf(stderr, "No model \"%s\" found.\n", model_name);
        }
        goto cleanup;
    }

    if (!task) {
        printf("%s features:\n", model->name);

        names = lys_features_list(model, &states);

        /* get the max len */
        max_len = 0;
        for (i = 0; names[i]; ++i) {
            if (strlen(names[i]) > max_len) {
                max_len = strlen(names[i]);
            }
        }
        for (i = 0; names[i]; ++i) {
            printf("\t%-*s (%s)\n", max_len, names[i], states[i] ? "on" : "off");
        }
        free(names);
        free(states);
        if (!i) {
            printf("\t(none)\n");
        }
    } else {
        feat_names = strtok(feat_names, ",");
        while (feat_names) {
            if (((task == 1) && lys_features_enable(model, feat_names))
                    || ((task == 2) && lys_features_disable(model, feat_names))) {
                fprintf(stderr, "Feature \"%s\" not found.\n", feat_names);
                ret = 1;
            }
            feat_names = strtok(NULL, ",");
        }
    }

cleanup:
    free(*argv);
    free(argv);

    return ret;
}

int
cmd_searchpath(const char *arg)
{
    const char *path;
    struct stat st;

    if (strchr(arg, ' ') == NULL) {
        fprintf(stderr, "Missing the search path.\n");
        return 1;
    }
    path = strchr(arg, ' ')+1;

    if (!strcmp(path, "-h") || !strcmp(path, "--help")) {
        cmd_searchpath_help();
        return 0;
    }

    if (stat(path, &st) == -1) {
        fprintf(stderr, "Failed to stat the search path (%s).\n", strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "\"%s\" is not a directory.\n", path);
        return 1;
    }

    free(search_path);
    search_path = strdup(path);

    ly_ctx_set_searchdir(ctx, search_path);

    return 0;
}

int
cmd_clear(const char *UNUSED(arg))
{
    ly_ctx_destroy(ctx);
    ctx = ly_ctx_new(search_path);
    if (!ctx) {
        fprintf(stderr, "Failed to create context.\n");
        return 1;
    }
    return 0;
}

int
cmd_verb(const char *arg)
{
    const char *verb;
    if (strlen(arg) < 5) {
        cmd_verb_help();
        return 1;
    }

    verb = arg + 5;
    if (!strcmp(verb, "error") || !strcmp(verb, "0")) {
        ly_verb(0);
    } else if (!strcmp(verb, "warning") || !strcmp(verb, "1")) {
        ly_verb(1);
    } else if (!strcmp(verb, "verbose")  || !strcmp(verb, "2")) {
        ly_verb(2);
    } else if (!strcmp(verb, "debug")  || !strcmp(verb, "3")) {
        ly_verb(3);
    } else {
        fprintf(stderr, "Unknown verbosity \"%s\"\n", verb);
        return 1;
    }

    return 0;
}

int
cmd_quit(const char *UNUSED(arg))
{
    done = 1;
    return 0;
}

int
cmd_help(const char *arg)
{
    int i;
    char *args = strdupa(arg);
    char *cmd = NULL;

    strtok(args, " ");
    if ((cmd = strtok(NULL, " ")) == NULL) {

generic_help:
        fprintf(stdout, "Available commands:\n");

        for (i = 0; commands[i].name; i++) {
            if (commands[i].helpstring != NULL) {
                fprintf(stdout, "  %-15s %s\n", commands[i].name, commands[i].helpstring);
            }
        }
    } else {
        /* print specific help for the selected command */

        /* get the command of the specified name */
        for (i = 0; commands[i].name; i++) {
            if (strcmp(cmd, commands[i].name) == 0) {
                break;
            }
        }

        /* execute the command's help if any valid command specified */
        if (commands[i].name) {
            if (commands[i].help_func != NULL) {
                commands[i].help_func();
            } else {
                printf("%s\n", commands[i].helpstring);
            }
        } else {
            /* if unknown command specified, print the list of commands */
            printf("Unknown command \'%s\'\n", cmd);
            goto generic_help;
        }
    }

    return 0;
}

COMMAND commands[] = {
        {"help", cmd_help, NULL, "Display commands description"},
        {"add", cmd_add, cmd_add_help, "Add a new model"},
        {"print", cmd_print, cmd_print_help, "Print model"},
        {"data", cmd_data, cmd_data_help, "Load, validate and optionally print complete datastore data"},
        {"config", cmd_config, cmd_config_help, "Load, validate and optionally print edit-config's data"},
        {"filter", cmd_filter, cmd_filter_help, "Load, validate and optionally print subtree filter data"},
        {"xpath", cmd_xpath, cmd_xpath_help, "Evaluate an XPath expression on a data tree"},
        {"list", cmd_list, cmd_list_help, "List all the loaded models"},
        {"feature", cmd_feature, cmd_feature_help, "Print/enable/disable all/specific features of models"},
        {"searchpath", cmd_searchpath, cmd_searchpath_help, "Set the search path for models"},
        {"clear", cmd_clear, NULL, "Clear the context - remove all the loaded models"},
        {"verb", cmd_verb, cmd_verb_help, "Change verbosity"},
        {"quit", cmd_quit, NULL, "Quit the program"},
        /* synonyms for previous commands */
        {"?", cmd_help, NULL, "Display commands description"},
        {"exit", cmd_quit, NULL, "Quit the program"},
        {NULL, NULL, NULL, NULL}
};
