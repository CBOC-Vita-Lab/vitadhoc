# Invoke alongside hostcore/Makefile, from gpsp/vita/hostcore.
LIBRARY_TEST := $(B)/library_test
LIBRARY_APP := $(filter-out $(B)/app/main_vita.o $(B)/app/psp/ui_psp.o,$(APP_OBJS))

$(B)/library_test.o: ../tests/library/library_test.c ../../psp/ui_psp.c ../ui_vita_home.h
	$(CC) $(APP_FLAGS) $(UI_WARN) -c -o $@ $<

$(B)/library_app_main.o: $(B)/app/main_vita.o
	$(OBJCOPY) --redefine-sym main=vitadhoc_app_main $< $@

$(LIBRARY_TEST): $(B)/library_test.o $(B)/library_app_main.o $(LIBRARY_APP) $(FE_OBJS) $(GB_OBJS) $(CORE_A) $(CORE_B) $(CORE_R)
	$(CC) $(CORE_ARCH_FLAGS) $(LDFLAGS_ARCH) $(SAN_FLAGS) -o $@ \
	  $(B)/library_test.o $(B)/library_app_main.o $(LIBRARY_APP) $(FE_OBJS) \
	  $(GB_OBJS) $(CORE_R) $(CORE_B) $(CORE_A) -lm -lpthread

library-test: $(LIBRARY_TEST)
	./$(LIBRARY_TEST)

.PHONY: library-test
