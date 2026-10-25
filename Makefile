MODULE_big = dynamic_ext
OBJS = dynamic_ext.o de_shim.o
EXTENSION = dynamic_ext
DATA = dynamic_ext--1.0.sql
PGFILEDESC = "dynamic_ext - a dynamic extension for PostgreSQL"
REGRESS = dynamic_ext

DE_ROOT = external/dynamic-extension
PG_CPPFLAGS = -isystem $(DE_ROOT)/include -isystem $(DE_ROOT)/external/psudb-common/cpp/include \
              -isystem $(DE_ROOT)/external/json/include -isystem $(DE_ROOT)/external/stduuid/include
PG_CXXFLAGS = -std=c++20
SHLIB_LINK = -lstdc++ -latomic -lgsl -lgslcblas

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
override with_llvm = no
include $(PGXS)

compile_commands.json:
	$(MAKE) clean
	bear -- $(MAKE)