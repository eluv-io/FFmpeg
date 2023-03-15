#!/bin/bash
# Check Ni xcoder basic functions

AVone_check=$(ffmpeg -version 2> /dev/null | grep 'ffmpeg version 3.4.2' 2> /dev/null)

# generate a YUV file of 1280x720p_Basketball.264 if needed
function gen_yuv_file_if_needed() {
    if [ ! -f "../libxcoder/test/1280x720p_Basketball.yuv" ]; then
        ./ffmpeg -nostdin -vsync 0 -y -i ../libxcoder/test/1280x720p_Basketball.264 -c:v rawvideo ../libxcoder/test/1280x720p_Basketball.yuv &> /dev/null
        if [[ $? != 0 ]]; then
            echo -e "\e[31mFAIL\e[0m: cannot generate ../libxcoder/test/1280x720p_Basketball.yuv from ../libxcoder/test/1280x720p_Basketball.264"
        fi
    fi
}

# check a file vs expected hash, print result
# $1 - file
# $2 - hash
function check_hash() {
    HASH=`md5sum ${1}`
    if [[ ${HASH%% *} == $2 ]]; then
        echo -e "\e[32mPASS: ${1} matches checksum.\e[0m"
    else
        echo -e "\e[31mFAIL: ${1} does not match checksum.\e[0m"
        echo -e "\e[31m      expected: ${2}\e[0m"
        echo -e "\e[31m      ${1}: ${HASH%% *}\e[0m"
    fi
}

# check a return code vs 0, print result
# $1 - rc
function check_rc() {
    if [[ $1 != 0 ]]; then
        echo -e "\e[31mFAIL: ${1} return code is ${1}\e[0m"
    fi
}

while true;do
options=("check pci device" "check nvme list" "rsrc_init" "ni_rsrc_mon" "test 264 decoder" "test 265 decoder" "test VP9 decoder" "test 264 encoder" "test 265 encoder" 
         "test AV1 encoder" "test 264->265 transcoder" "test 264->AV1 transcoder" "test 265->264 transcoder" "test 265->AV1 transcoder" "test VP9->264 transcoder" 
         "test VP9->265 transcoder" "test VP9->AV1 transcoder" "Quit")
echo -e "\e[33mChoose an option:\e[0m"
select opt in "${options[@]}"
do
    case $opt in
        "check pci device")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            sudo lspci -d 1d82:
            check_rc $?
            echo
            break
        ;;
        "check nvme list")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            sudo nvme list
            check_rc $?
            echo
            break
        ;;
        "rsrc_init")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ../libxcoder/build/init_rsrc
            check_rc $?
            echo
            break
        ;;
        "ni_rsrc_mon")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ../libxcoder/build/ni_rsrc_mon
            check_rc $?
            echo
            break
        ;;
        "test 264 decoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Decoding H.264 to YUV with NI XCoder (full log) #####
            output_file="output_5.yuv"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_quadra_dec -i ../libxcoder/test/1280x720p_Basketball.264 -c:v rawvideo ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="be2e62fc528c61a01ac44eae5518e13a"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 265 decoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Decoding H.265 to YUV with NI XCoder (full log) #####
            output_file="output_6.yuv"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h265_ni_quadra_dec -i ../libxcoder/test/akiyo_352x288p25.265 -c:v rawvideo ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="f5a29fd3fd2581844848519bafd7370d"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test VP9 decoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Decoding VP9 to YUV with NI XCoder (full log) #####
            output_file="output_7.yuv"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v vp9_ni_quadra_dec -i ../libxcoder/test/akiyo_352x288p25_300.ivf -c:v rawvideo ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="0da8a892f4f835cd8d8f0c02f208e1f6"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 264 encoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            gen_yuv_file_if_needed
            ##### Encoding YUV to H.264 with NI XCoder (full log) #####
            output_file="output_8.h264"
            cmd="ffmpeg -y -hide_banner -nostdin -f rawvideo -pix_fmt yuv420p -s:v 1280x720 -r 25 -i ../libxcoder/test/1280x720p_Basketball.yuv -c:v h264_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="62872b29f54af7ddddd93dcf2e0d94b7"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 265 encoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            gen_yuv_file_if_needed
            ##### Encoding YUV to H.265 with NI XCoder (full log) #####
            output_file="output_9.h265"
            cmd="ffmpeg -y -hide_banner -nostdin -f rawvideo -pix_fmt yuv420p -s:v 1280x720 -r 25 -i ../libxcoder/test/1280x720p_Basketball.yuv -c:v h265_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="8a86c89d4e29359ed8072482f7d81ef6"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test AV1 encoder")
            if [[ $AVone_check ]]; then
                echo -e "\e[31m AV1 cannot be run on 3.4.2, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            gen_yuv_file_if_needed
            ##### Encoding YUV to AV1 with NI XCoder (full log) #####
            output_file="output_10.ivf"
            cmd="ffmpeg -y -hide_banner -nostdin -f rawvideo -pix_fmt yuv420p -s:v 1280x720 -r 25 -i ../libxcoder/test/1280x720p_Basketball.yuv -c:v av1_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="d1ca90ddce59e324dd4b827bfd73ce0d"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 264->265 transcoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.264 to H.265 with NI XCoder (full log) #####
            output_file="output_11.h265"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_quadra_dec -i ../libxcoder/test/1280x720p_Basketball.264 -c:v h265_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="e460dc73f8e5daff3c0b7e271e09553b"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 264->AV1 transcoder")
            if [[ $AVone_check ]]; then
                echo -e "\e[31m AV1 cannot be run on 3.4.2, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.264 to AV1 with NI XCoder (full log) #####
            output_file="output_12.ivf"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_quadra_dec -i ../libxcoder/test/1280x720p_Basketball.264 -c:v av1_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="be0c5eab2c6c55b8e9c4f31ba41a40e1"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 265->264 transcoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.265 to H.264 with NI XCoder (full log) #####
            output_file="output_13.h264"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h265_ni_quadra_dec -i ../libxcoder/test/akiyo_352x288p25.265 -c:v h264_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="7ab8b4d619ec4f7c19af2400588310df"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 265->AV1 transcoder")
            if [[ $AVone_check ]]; then
                echo -e "\e[31m AV1 cannot be run on 3.4.2, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.265 to AV1 with NI XCoder (full log) #####
            output_file="output_14.ivf"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h265_ni_quadra_dec -i ../libxcoder/test/akiyo_352x288p25.265 -c:v av1_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="e0c850b349887296f0dd4a5db5c80f6d"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test VP9->264 transcoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding VP9 to H.264 with NI XCoder (full log) #####
            output_file="output_15.h264"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v vp9_ni_quadra_dec -i ../libxcoder/test/akiyo_352x288p25_300.ivf -c:v h264_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="6d603f169e841db7c5a260e1eac11904"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test VP9->265 transcoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding VP9 to H.265 with NI XCoder (full log) #####
            output_file="output_16.h265"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v vp9_ni_quadra_dec -i ../libxcoder/test/akiyo_352x288p25_300.ivf -c:v h265_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="23905d53b2f0290a11963fefafd1988b"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test VP9->AV1 transcoder")
            if [[ $AVone_check ]]; then
                echo -e "\e[31m AV1 cannot be run on 3.4.2, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding VP9 to H.265 with NI XCoder (full log) #####
            output_file="output_17.ivf"
            cmd="ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v vp9_ni_quadra_dec -i ../libxcoder/test/akiyo_352x288p25_300.ivf -c:v av1_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            echo -e "\e[33mComplete! ${output_file} has been generated.\e[0m"
            CHECKSUM="308427dfe7b908c099901b809e03f7e2"
            check_hash "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "Quit")
            break 2
        ;;
        *)
            echo -e "\e[31mInvalid choice!\e[0m"
            echo
            break
        ;;
    esac
done
done
echo -e "\e[33mBye!\e[0m"
echo
