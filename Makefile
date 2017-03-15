SUBDIRS = libhrd \
	ws-sequencer atomics-sequencer ss-sequencer herd mica \
	rw-tput-sender rw-tput-receiver rw-allsig \
	ud-sender ud-receiver \
	rw-allsig rw-postlist-latency

CLEANDIRS = $(SUBDIRS:%=clean-%)

.PHONY: subdirs clean $(SUBDIRS) $(CLEANDIRS)

subdirs: $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@
                                                                                    
clean: $(CLEANDIRS)
$(CLEANDIRS):
	$(MAKE) -C $(@:clean-%=%) clean
