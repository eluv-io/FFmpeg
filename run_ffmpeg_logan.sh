#!/bin/bash
# Check Ni xcoder basic functions

# generate a YUV file of 1280x720p_Basketball.264 if needed
function gen_yuv_file_if_needed() {
    if [ ! -f "../libxcoder_logan/test/1280x720p_Basketball.yuv" ]; then
        ./ffmpeg -nostdin -vsync 0 -y -i ../libxcoder_logan/test/1280x720p_Basketball.264 -c:v rawvideo ../libxcoder_logan/test/1280x720p_Basketball.yuv &> /dev/null
        if [[ $? != 0 ]]; then
            echo -e "\e[31mFAIL\e[0m: cannot generate ../libxcoder_logan/test/1280x720p_Basketball.yuv from ../libxcoder_logan/test/1280x720p_Basketball.264"
        fi
    fi
}

#for some clis output is linked to the root directory
declare -a Outputs=("output_5.yuv" "output_6.yuv" "output_7.h264" "output_8.h265" "output_9.h265")
for i in "${Outputs[@]}"
do
    rm -f $i
done

while true;do
options=("check pci device" "check nvme list" "rsrc_init" "ni_rsrc_mon" "test 264 decoder" "test 265 decoder" "test 264 encoder" "test 265 encoder" "test 264->265 transcoder" "Quit")
echo -e "\e[31mChoose an option:\e[0m"
select opt in "${options[@]}"
do
    case $opt in
        "check pci device")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            lspci -d 1d82:
            echo
            break
        ;;
        "check nvme list")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            sudo nvme list
            echo
            break
        ;;
        "rsrc_init")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            ../libxcoder_logan/bin/init_rsrc_logan
            echo
            break
        ;;
        "ni_rsrc_mon")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            ../libxcoder_logan/bin/ni_rsrc_mon_logan
            echo
            break
        ;;
        "test 264 decoder")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            ##### Decoding H.264 to YUV with NI XCoder (full log) #####
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_logan_dec -i ../libxcoder_logan/test/1280x720p_Basketball.264 -c:v rawvideo output_5.yuv"
            echo $cmd
            $cmd 2>&1 | tee ffmpeg_5.log
            echo -e "\e[31mComplete! output_5.yuv has been generated.\e[0m"
            CHECKSUM="be2e62fc528c61a01ac44eae5518e13a"
            HASH=`md5sum output_5.yuv`
            if [[ ${HASH%% *} == $CHECKSUM ]]; then
                echo -e "\e[31mPASS: output_5.yuv matches checksum.\e[0m"
            else
                echo -e "\e[31mFAIL: output_5.yuv does not match checksum.\e[0m"
            fi
            echo
            break
        ;;
        "test 265 decoder")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            ##### Decoding H.265 to YUV with NI XCoder (full log) #####
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h265_ni_logan_dec -i ../libxcoder_logan/test/akiyo_352x288p25.265 -c:v rawvideo output_6.yuv"
            echo $cmd
            $cmd 2>&1 | tee ffmpeg_6.log
            echo -e "\e[31mComplete! output_6.yuv has been generated.\e[0m"
            CHECKSUM="f5a29fd3fd2581844848519bafd7370d"
            HASH=`md5sum output_6.yuv`
            if [[ ${HASH%% *} == $CHECKSUM ]]; then
                echo -e "\e[31mPASS: output_6.yuv matches checksum.\e[0m"
            else
                echo -e "\e[31mFAIL: output_6.yuv does not match checksum.\e[0m"
            fi
            echo
            break
        ;;
        "test 264 encoder")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            gen_yuv_file_if_needed
            ##### Encoding YUV to H.264 with NI XCoder (full log) #####
            cmd="./ffmpeg -y -hide_banner -nostdin -f rawvideo -pix_fmt yuv420p -s:v 1280x720 -r 25 -i ../libxcoder_logan/test/1280x720p_Basketball.yuv -c:v h264_ni_logan_enc output_7.h264"
            echo $cmd
            $cmd 2>&1 | tee ffmpeg_7.log
            echo -e "\e[31mComplete! output_7.h264 has been generated.\e[0m"
            CHECKSUM="6713d8cc54cc4d0ab0b912a54338e4ee"
            HASH=`md5sum output_7.h264`
            if [[ ${HASH%% *} == $CHECKSUM ]]; then
                echo -e "\e[31mPASS: output_7.h264 matches checksum.\e[0m"
            else
                echo -e "\e[31mFAIL: output_7.h264 does not match checksum.\e[0m"
            fi
            echo
            break
        ;;
        "test 265 encoder")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            gen_yuv_file_if_needed
            ##### Encoding YUV to H.265 with NI XCoder (full log) #####
            cmd="./ffmpeg -y -hide_banner -nostdin -f rawvideo -pix_fmt yuv420p -s:v 1280x720 -r 25 -i ../libxcoder_logan/test/1280x720p_Basketball.yuv -c:v h265_ni_logan_enc output_8.h265"
            echo $cmd
            $cmd 2>&1 | tee ffmpeg_8.log
            echo -e "\e[31mComplete! output_8.h265 has been generated.\e[0m"
            CHECKSUM="f13466948494cbc1217892f55f60f5ff"
            HASH=`md5sum output_8.h265`
            if [[ ${HASH%% *} == $CHECKSUM ]]; then
                echo -e "\e[31mPASS: output_8.h265 matches checksum.\e[0m"
            else
                echo -e "\e[31mFAIL: output_8.h265 does not match checksum.\e[0m"
            fi
            echo
            break
        ;;
        "test 264->265 transcoder")
            echo -e "\e[31mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.264 to H.265 with NI XCoder (full log) #####
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_logan_dec -i ../libxcoder_logan/test/1280x720p_Basketball.264 -c:v h265_ni_logan_enc output_9.h265"
            echo $cmd
            $cmd 2>&1 | tee ffmpeg_9.log
            echo -e "\e[31mComplete! output_9.h265 has been generated.\e[0m"
            CHECKSUM="171a75a81ae8a152fa9fb182099629bf"
            HASH=`md5sum output_9.h265`
            echo ${HASH%% *}
            if [[ ${HASH%% *} == $CHECKSUM ]]; then
                echo -e "\e[31mPASS: output_9.h265 matches checksum.\e[0m"
            else
                echo -e "\e[31mFAIL: output_9.h265 does not match checksum.\e[0m"
            fi
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
echo -e "\e[31mBye!\e[0m"
echo
