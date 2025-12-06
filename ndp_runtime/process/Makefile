# 컴파일러 설정
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Iinclude -lpthread -ldl -lrt -g

# 라이브러리 디렉토리 설정 (기존 빌드 방식 반영)
LIB_DIR = /home/doogunwo/Desktop/tokenizers-0.20.3/tokenizers/tokenizers-cpp/build
LIBS = -L$(LIB_DIR) -ltokenizers_cpp -ltokenizers_c -lpthread -ldl

# 디렉토리 설정
SRC_DIR = src/
OBJ_DIR = bin/obj
BIN_DIR = bin
MODEL_DIR = model

# 소스 파일 목록
SRCS = $(wildcard $(SRC_DIR)/*.cpp)

# 오브젝트 파일 목록
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

# 실행 파일
TARGET = $(BIN_DIR)/bpe_process

# 기본 빌드 타겟
all: $(TARGET)

# 실행 파일 빌드 규칙 (링크 시 추가 라이브러리 적용)
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

# 개별 오브젝트 파일 빌드 규칙
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 클린업 (오브젝트 파일 및 실행 파일 제거)
clean:
	rm -rf $(OBJ_DIR)/*.o $(TARGET)

# 공유 메모리 삭제 (디버깅을 위한 추가 명령어)
clean_shm:
	ipcrm -M 5678
	ipcrm -M 5679
