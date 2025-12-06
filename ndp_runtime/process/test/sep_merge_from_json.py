import json

# JSON 파일 로드
with open("../model/byte_level_bpe_model.json", "r", encoding="utf-8") as f:
    data = json.load(f)

# "merges" 데이터 추출
if "model" in data and "merges" in data["model"]:
    merges = data["model"]["merges"]
    
    # merges가 리스트인지 확인
    if isinstance(merges, list):
        # merges.txt 파일로 저장
        with open("../model/merges.txt", "w", encoding="utf-8") as f:
            for merge in merges:
                if isinstance(merge, list):  # 리스트인 경우
                    f.write(" ".join(merge) + "\n")  # ✅ 리스트를 문자열로 변환 후 저장
                else:
                    f.write(str(merge) + "\n")  # 문자열인 경우 그대로 저장

        print("✅ 'merges.txt' 파일이 성공적으로 생성되었습니다.")
    else:
        print("🚨 오류: 'merges' 값이 리스트가 아닙니다.")
else:
    print("🚨 오류: JSON 파일에서 'model' 또는 'merges' 키를 찾을 수 없습니다.")
