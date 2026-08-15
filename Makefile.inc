LIBS := -llunaservice  -lmojoluna -lmojodb -lmojocore -lpurple -lsanitize
LOCAL_LDFLAGS := $(LDFLAGS) $(LIBS)

SOURCE_DIRS := src

# Every translation unit under src/ — same rule as CMakeLists.txt and build.sh,
# so a new handler only has to be dropped in src/ to land in all three builds.
IM_SOURCES := $(notdir $(wildcard src/*.cpp))

IM_TARGET := $(OBJDIR)/imlibpurpletransport
IM_OBJECTS := $(IM_SOURCES:%.cpp=$(OBJDIR)/%.o)

all: setup $(IM_TARGET)
		
$(IM_TARGET): $(IM_OBJECTS) 
	$(CXX) -o $(IM_TARGET) $(IM_OBJECTS) $(LOCAL_LDFLAGS) 
	
$(OBJDIR)/%.o: %.cpp
	$(CXX) -MMD $(INCLUDES) $(LOCAL_CFLAGS) $(LOCAL_CPPFLAGS) -c $< -o $@

setup: $(OBJDIR)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

vpath %.cpp $(SOURCE_DIRS)

-include $(IM_SOURCES:%.cpp=$(OBJDIR)/%.d)
	
clean:
	rm -rf $(OBJDIR)
	rm -rf doc/html
	
.PHONY: clean all setup docs
