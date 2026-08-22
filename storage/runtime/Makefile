# 컴파일러 설정
CXX = g++
OPTFLAGS ?= -O3 -DNDEBUG
DBGFLAGS ?= -g
CXXFLAGS = -std=c++17 -Wall -Wextra -Iinclude -lpthread -ldl -lrt $(OPTFLAGS) $(DBGFLAGS)

# 라이브러리 디렉토리 설정 (NDT-BPE 내부 토크나이저 의존)
TOKENIZERS_DIR = ../../compute/third_party/tokenizers-cpp
LIB_DIR = $(TOKENIZERS_DIR)/build
LIBS = -L$(LIB_DIR) -ltokenizers_cpp -ltokenizers_c -lpthread -ldl
RPATH = -Wl,-rpath,$(abspath $(LIB_DIR))

# 디렉토리 설정
SRC_DIR = src/
OBJ_DIR = bin/obj
BIN_DIR = bin
MODEL_DIR = model

# 소스 파일 목록
SRCS = $(filter-out $(SRC_DIR)/main2.cpp $(SRC_DIR)/bpe_monitor.cpp, $(wildcard $(SRC_DIR)/*.cpp))

# 오브젝트 파일 목록
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

# 실행 파일
TARGET = $(BIN_DIR)/bpe_process
TARGET_MAIN2 = $(BIN_DIR)/bpe_process_main2
TARGET_MON = $(BIN_DIR)/bpe_monitor

# 기본 빌드 타겟
all: $(TARGET) $(TARGET_MAIN2) $(TARGET_MON)

# 실행 파일 빌드 규칙 (링크 시 추가 라이브러리 적용)
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(RPATH)

$(TARGET_MAIN2): $(OBJ_DIR)/main2.o $(OBJ_DIR)/common.o $(OBJ_DIR)/bpe_tokenizer.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(RPATH)

$(TARGET_MON): $(OBJ_DIR)/bpe_monitor.o
	$(CXX) $(CXXFLAGS) $^ -o $@

# 개별 오브젝트 파일 빌드 규칙
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 클린업 (오브젝트 파일 및 실행 파일 제거)
clean:
	rm -rf $(OBJ_DIR)/*.o $(TARGET) $(TARGET_MAIN2) $(TARGET_MON)

# 공유 메모리 삭제 (디버깅을 위한 추가 명령어)
clean_shm:
	@for key in $$(printf "0x%x\n" $$(seq $$((0x1000)) $$((0x103f)))); do ipcrm -M $$key 2>/dev/null || true; done
	@for key in $$(printf "0x%x\n" $$(seq $$((0x1200)) $$((0x123f)))); do ipcrm -M $$key 2>/dev/null || true; done
	@for key in $$(printf "0x%x\n" $$(seq $$((0x1400)) $$((0x143f)))); do ipcrm -M $$key 2>/dev/null || true; done
	@for key in $$(printf "0x%x\n" $$(seq $$((0x1600)) $$((0x163f)))); do ipcrm -M $$key 2>/dev/null || true; done
	@for bank in $$(seq 0 15); do for root in 0x3000 0x4000; do base=$$((root + bank * 0x100)); for key in $$(seq $$base $$((base + 0x3f))); do ipcrm -M $$key 2>/dev/null || true; done; done; done
	@ipcrm -M 0x2000 2>/dev/null || true
	@ipcrm -M 0x2100 2>/dev/null || true
	@ipcrm -M 0x2200 2>/dev/null || true
	@ipcrm -Q 1002 2>/dev/null || true
	@ipcrm -Q 1003 2>/dev/null || true
	@rm -f /var/tmp/ndt_bpe_eventfd.sock
