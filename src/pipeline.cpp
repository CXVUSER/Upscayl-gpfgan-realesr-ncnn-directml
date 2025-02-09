﻿// Face restore pipeline

#include "include/pipeline.h"
#include <numeric>

#if defined(_WIN32)
extern unsigned char *wic_decode_image(const wchar_t *filepath, int *w, int *h, int *c);
extern int wic_encode_image(const wchar_t *filepath, int w, int h, int c, void *bgrdata);
#include "include\helpers.h"
#endif

PipeLine::PipeLine() {
}
PipeLine::~PipeLine() {}

static void to_ocv(const ncnn::Mat &result, cv::Mat &out) {
    cv::Mat cv_result_32F = cv::Mat::zeros(cv::Size(512, 512), CV_32FC3);
    for (int i = 0; i < result.h; i++) {
        for (int j = 0; j < result.w; j++) {
            cv_result_32F.at<cv::Vec3f>(i, j)[2] = (result.channel(0)[i * result.w + j] + 1) / 2;
            cv_result_32F.at<cv::Vec3f>(i, j)[1] = (result.channel(1)[i * result.w + j] + 1) / 2;
            cv_result_32F.at<cv::Vec3f>(i, j)[0] = (result.channel(2)[i * result.w + j] + 1) / 2;
        }
    }

    cv::Mat cv_result_8U;
    cv_result_32F.convertTo(cv_result_8U, CV_8UC3, 255.0, 0);

    cv_result_8U.copyTo(out);
};

void PipeLine::paste_faces_to_input_image(const cv::Mat &restored_face, cv::Mat &trans_matrix_inv, cv::Mat &bg_upsample) {

    if (pipe.custom_scale) {
        trans_matrix_inv.at<double>(0, 2) += (double) pipe.custom_scale / 2;
        trans_matrix_inv.at<double>(1, 2) += (double) pipe.custom_scale / 2;
    } else {
        trans_matrix_inv.at<double>(0, 2) += (double) pipe.model_scale / 2;
        trans_matrix_inv.at<double>(1, 2) += (double) pipe.model_scale / 2;
    }

    cv::Mat upscaled_face;
    double ups_f = 0.0;
    if (pipe.face_upsample && !pipe.fc_up_model.empty()) {
        ups_f = 4.0;
        fprintf(stderr, "Upsample face start...\n");
        face_up_NCNN_.scale = 4;
        face_up_NCNN_.prepadding = 10;
        face_up_NCNN_.tilesize = 200;

        int w{}, h{}, c{};
#if defined(_WIN32)
        void *pixeldata = wic_decode_image(L"output.png", &w, &h, &c);
#else
#endif
        ncnn::Mat bg_presample(w, h, (void *) pixeldata, (size_t) c, c);
        ncnn::Mat bg_upsamplencnn(w * 4, h * 4, (size_t) c, c);
        std::wstringstream str_param1;
        str_param1 << "out.png" << std::ends;
        face_up_NCNN_.process(bg_presample, bg_upsamplencnn);
#if defined(_WIN32)
        wic_encode_image(str_param1.view().data(), w * 4, h * 4, 3, bg_upsamplencnn.data);
#else
#endif

        upscaled_face = cv::imread("out.png", cv::ImreadModes::IMREAD_COLOR_BGR);

        trans_matrix_inv /= ups_f;
        trans_matrix_inv.at<double>(0, 2) *= ups_f;
        trans_matrix_inv.at<double>(1, 2) *= ups_f;

        fprintf(stderr, "Upsample face finish...\n");
        if (pixeldata)
            free(pixeldata);
    }

    cv::Mat inv_restored;
    if (pipe.face_upsample)
        cv::warpAffine(upscaled_face, inv_restored, trans_matrix_inv, bg_upsample.size(),
                       cv::InterpolationFlags::INTER_LINEAR, cv::BorderTypes::BORDER_CONSTANT);
    else
        cv::warpAffine(restored_face, inv_restored, trans_matrix_inv, bg_upsample.size(),
                       cv::InterpolationFlags::INTER_LINEAR, cv::BorderTypes::BORDER_CONSTANT);

    cv::Size ms_size;

    if (pipe.face_upsample)
        ms_size = cv::Size(512 * ups_f, 512 * ups_f);
    else
        ms_size = cv::Size(512, 512);

    cv::Mat mask = cv::Mat::ones(ms_size, CV_8UC1) * 255;
    cv::Mat inv_mask;
    cv::warpAffine(mask, inv_mask, trans_matrix_inv, bg_upsample.size(),
                   cv::InterpolationFlags::INTER_LINEAR, cv::BorderTypes::BORDER_CONSTANT);


    cv::Size krn_size;
    if (pipe.face_upsample)
        krn_size = cv::Size(4 * ups_f, 4 * ups_f);
    else
        krn_size = cv::Size(4, 4);

    cv::Mat inv_mask_erosion;
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, krn_size);
    cv::erode(inv_mask, inv_mask_erosion, kernel);
    cv::Mat pasted_face;
    cv::bitwise_and(inv_restored, inv_restored, pasted_face, inv_mask_erosion);

    int total_face_area = cv::countNonZero(inv_mask_erosion);
    int w_edge = int(std::sqrt(total_face_area) / 20);
    int erosion_radius = w_edge * 2;
    cv::Mat inv_mask_center;
    kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(erosion_radius, erosion_radius));
    cv::erode(inv_mask_erosion, inv_mask_center, kernel);

    int blur_size = w_edge * 2;
    cv::Mat inv_soft_mask;
    cv::GaussianBlur(inv_mask_center, inv_soft_mask, cv::Size(blur_size + 1, blur_size + 1),
                     0, 0, cv::BorderTypes::BORDER_DEFAULT);

    cv::Mat inv_soft_mask_f;
    inv_soft_mask.convertTo(inv_soft_mask_f, CV_32F, 1 / 255.f, 0.f);

#pragma omp parallel for
    for (int h = 0; h < bg_upsample.rows; ++h) {
        cv::Vec3b *img_ptr = bg_upsample.ptr<cv::Vec3b>(h);
        cv::Vec3b *face_ptr = pasted_face.ptr<cv::Vec3b>(h);
        float *mask_ptr = inv_soft_mask_f.ptr<float>(h);
        for (int w = 0; w < bg_upsample.cols; ++w) {
            img_ptr[w][0] = img_ptr[w][0] * (1 - mask_ptr[w]) + face_ptr[w][0] * mask_ptr[w];
            img_ptr[w][1] = img_ptr[w][1] * (1 - mask_ptr[w]) + face_ptr[w][1] * mask_ptr[w];
            img_ptr[w][2] = img_ptr[w][2] * (1 - mask_ptr[w]) + face_ptr[w][2] * mask_ptr[w];
        }
    }
}


int PipeLine::CreatePipeLine(PipelineConfig_t &pipeline_config) {
    pipe = pipeline_config;

    if (true == pipeline_config.ncnn) {
        if (pipe.codeformer) {
            int ret = codeformer_NCNN_.Load(pipe.model_path);
            if (ret < 0) {
                return -1;
            }
        } else {
            fprintf(stderr, "Loading GFPGANCleanv1-NoCE-C2 model from /models/GFPGANCleanv1-NoCE-C2-*...\n");
            gfpgan_NCNN_.load(pipe.model_path);
            fprintf(stderr, "Loading GFPGANCleanv1-NoCE-C2 model finished...\n");
        }
    }

    if (pipe.custom_scale)
        face_detector_NCNN_.setScale(pipe.custom_scale);
    else
        face_detector_NCNN_.setScale(pipe.model_scale);

    face_detector_NCNN_.setThreshold(pipe.prob_thr, pipe.nms_thr);

    int ret = face_detector_NCNN_.Load(pipe.model_path);
    if (ret < 0) {
        return -1;
    }

    if (pipe.face_upsample) {
        std::wstringstream str_param;
        str_param << pipe.fc_up_model << ".param" << std::ends;
        std::wstringstream str_bin;
        str_bin << pipe.fc_up_model << ".bin" << std::ends;
        face_up_NCNN_.load(str_param.view().data(), str_bin.view().data());
    }

    return 0;
}

cv::Mat preprocessImage(const cv::Mat &inputImage) {
    cv::Mat img;

    // 1. Приведение к типу float32 (если изображение загружено, например, через imread, то оно имеет тип CV_8UC3)
    inputImage.convertTo(img, CV_32F);

    //// 2. (Опционально) Изменяем размер до 512x512, если требуется
    // cv::resize(img, img, cv::Size(512, 512), 0, 0, cv::INTER_LINEAR);

    //// 3. Масштабирование в диапазон [0,1]
    img = img / 255.0;

    //// 4. Конвертация из BGR в RGB
    cv::cvtColor(img, img, cv::COLOR_BGR2RGB);

    //// 5. Создание blob: cv::dnn::blobFromImage выполняет автоматическую перестановку из HWC в CHW
    ////    Здесь scaleFactor равен 1.0, поскольку мы уже делим на 255.
    ////    Параметр swapRB установлен в false, так как мы уже перевели в RGB.
    cv::Mat blob = cv::dnn::blobFromImage(img, 1.0, cv::Size(), cv::Scalar(), false, false, CV_32F);
    // Теперь blob имеет форму [1, 3, H, W] (NCHW)

    //// 6. Нормализация: операция (img - 0.5)/0.5 равносильна (2*img - 1)
    ////    Применяем нормализацию по всему blob
    blob = blob * 2.0 - 1.0;

    //// 7. Blob уже имеет размерность батча (N=1), поэтому дополнительных действий не требуется.
    return blob;
}

// Функция postProcessImage преобразует выходной тензор модели в cv::Mat.
cv::Mat postProcessImage(const float *outputData, const std::vector<int64_t> &outputShape) {
    // Ожидаем, что выход имеет форму [N, C, H, W]
    if (outputShape.size() != 4) {
        throw std::runtime_error("Expected output tensor shape with 4 dimensions (N, C, H, W).");
    }

    int N = static_cast<int>(outputShape[0]);
    int C = static_cast<int>(outputShape[1]);
    int H = static_cast<int>(outputShape[2]);
    int W = static_cast<int>(outputShape[3]);

    if (N != 1) {
        throw std::runtime_error("postProcessImage supports only batch size of 1.");
    }
    if (C != 3) {
        throw std::runtime_error("postProcessImage supports only 3-channel output.");
    }

    // Размер каждого канала
    size_t channelSize = static_cast<size_t>(H * W);

    // Извлекаем данные по каналам и создаём для каждого канал cv::Mat.
    // Здесь предполагается, что данные расположены подряд: сначала весь первый канал, затем второй, затем третий.
    std::vector<cv::Mat> channels;
    for (int i = 0; i < C; i++) {
        // Создаем заголовок для матрицы, без копирования данных (для безопасности делаем clone ниже)
        cv::Mat channel(H, W, CV_32F, const_cast<float *>(outputData + i * channelSize));
        channels.push_back(channel.clone());// clone гарантирует непрерывность данных
    }

    // Объединяем каналы в одно изображение формата HWC (RGB)
    cv::Mat image;
    cv::merge(channels, image);

    // Преобразуем значения из диапазона [-1, 1] в [0, 255].
    // Формула: image_out = (image + 1) / 2 * 255
    image = (image + 1.0f) / 2.0f * 255.0f;
    image.convertTo(image, CV_8UC3);

    // Если требуется, преобразуем изображение из RGB в BGR для корректного отображения/сохранения OpenCV
    cv::cvtColor(image, image, cv::COLOR_RGB2BGR);

    return image;
}

static void RunModel(
        const std::filesystem::path &modelPath,
        const std::filesystem::path &imagePath,
        PipelineConfig_t &pipe) {

    Ort::SessionOptions sessionOptions;

    const OrtApi &ortApi = Ort::GetApi();

#if defined(_WIN32)
    // DML execution provider prefers these session options.
    sessionOptions.DisableMemPattern();
    sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    // By passing in an explicitly created DML device & queue, the DML execution provider sends work
    // to the desired device. If not used, the DML execution provider will create its own device & queue.
    auto [dmlDevice, d3dQueue] = CreateDmlDeviceAndCommandQueue("");
    const OrtDmlApi *ortDmlApi = nullptr;
    Ort::ThrowOnError(ortApi.GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void **>(&ortDmlApi)));
    Ort::ThrowOnError(ortDmlApi->SessionOptionsAppendExecutionProvider_DML1(
            sessionOptions,
            dmlDevice.Get(),
            d3dQueue.Get()));
#else if defined(__linux__)
    Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CUDA(sessionOptions, 0));
#endif

    // Load ONNX model into a session.
    Ort::Env env(OrtLoggingLevel::ORT_LOGGING_LEVEL_WARNING, "UPS_CDF");
    Ort::Session ortSession(env, modelPath.wstring().c_str(), sessionOptions);

    Ort::AllocatorWithDefaultOptions ortAllocator;

    auto inputName = ortSession.GetInputNameAllocated(0, ortAllocator);
    auto inputTypeInfo = ortSession.GetInputTypeInfo(0);
    auto inputTensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
    auto inputShape = inputTensorInfo.GetShape();

    auto outputName = ortSession.GetOutputNameAllocated(0, ortAllocator);
    auto outputTypeInfo = ortSession.GetOutputTypeInfo(0);
    auto outputTensorInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
    auto outputShape = outputTensorInfo.GetShape();

    cv::Mat img = cv::imread(imagePath.string().c_str(), cv::ImreadModes::IMREAD_COLOR_BGR);
    cv::Mat imgp = preprocessImage(img);
    float inputTensorSize = 1;
    for (auto dim: inputShape) {
        inputTensorSize *= static_cast<float>(dim);
    }
    // For simplicity, this sample binds input/output buffers in system memory instead of DirectX resources.
    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto imageTensor = Ort::Value::CreateTensor<float>(
            memoryInfo,
            (float *) imgp.data,
            inputTensorSize,
            inputShape.data(),
            inputShape.size());

    auto bindings = Ort::IoBinding::IoBinding(ortSession);

    if (pipe.codeformer) {
        // ===== Создаём второй входной тензор (fidelity) =====
        // Форма для fidelity – одномерный тензор с одним элементом.
        std::array<int64_t, 1> fidelityShape = {1};
        std::vector<double> fidelityValue{pipe.w};
        auto fidelityTensor = Ort::Value::CreateTensor(
                memoryInfo,
                fidelityValue.data(),
                fidelityValue.size() * sizeof(double),
                fidelityShape.data(),
                fidelityShape.size(),
                ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE);
        auto fidelityName = ortSession.GetInputNameAllocated(1, ortAllocator);
        bindings.BindInput(fidelityName.get(), fidelityTensor);
    }

    bindings.BindInput(inputName.get(), imageTensor);
    bindings.BindOutput(outputName.get(), memoryInfo);

    // Run the session to get inference results.
    Ort::RunOptions runOpts;
    ortSession.Run(runOpts, bindings);
    bindings.SynchronizeOutputs();

    cv::imwrite("output.png", postProcessImage((float *) bindings.GetOutputValues()[0].GetTensorRawData(), outputShape));
}

int PipeLine::Apply(const cv::Mat &input_img, cv::Mat &output_img) {
    PipeResult_t pipe_result;
    fprintf(stderr, "Detecting faces...\n");
    face_detector_NCNN_.Process(input_img, (void *) &pipe_result);
    fprintf(stderr, "Detected %d faces\n", pipe_result.face_count);

    char d[_MAX_PATH];
    sprintf(d, "%.1f", pipe.w);
    *strrchr(d, ',') = '.';

    for (int i = 0; i != pipe_result.face_count; ++i) {
        std::stringstream str;
        str << pipe.name << "_" << i + 1 << "_crop.png" << std::ends;
        cv::imwrite(str.view().data(), pipe_result.object[i].trans_img);
        if (pipe.codeformer) {
            fprintf(stderr, "Codeformer process %d face...\n", i + 1);
            std::stringstream str3;
            str3 << pipe.name << "_" << i + 1 << "_" << pipe.w << "_codeformer_crop.png" << std::ends;

            if (pipe.onnx) {
                RunModel(
                        "./models/codeformer_0_1_0.onnx",
                        str.view().data(), pipe);
                cv::Mat restored_face = cv::imread("output.png", cv::ImreadModes::IMREAD_COLOR_BGR);
                cv::imwrite(str3.view().data(), restored_face);
                fprintf(stderr, "Paste %d face in photo...\n", i + 1);
                paste_faces_to_input_image(restored_face, pipe_result.object[i].trans_inv, output_img);
            }
            if (pipe.ncnn) {
                CodeFormerResult_t res;
                pipe_result.codeformer_result.push_back(res);
                codeformer_NCNN_.Process(pipe_result.object[i].trans_img, pipe_result.codeformer_result[i]);
                cv::imwrite(str3.view().data(), pipe_result.codeformer_result[i].restored_face);
                fprintf(stderr, "Paste %d face in photo...\n", i + 1);
                paste_faces_to_input_image(pipe_result.codeformer_result[i].restored_face, pipe_result.object[i].trans_inv, output_img);
            }
        } else {
            fprintf(stderr, "%s process %d face...\n", getfilea((char *) pipe.face_model.c_str()), i + 1);
            std::stringstream str3;
            str3 << pipe.name << "_" << i + 1 << "_" << getfilea((char *) pipe.face_model.c_str()) << "_crop.png" << std::ends;
            if (pipe.onnx) {
                RunModel(
                        pipe.face_model,
                        str.view().data(), pipe);
                cv::Mat restored_face = cv::imread("output.png", cv::ImreadModes::IMREAD_COLOR_BGR);
                cv::imwrite(str3.view().data(), restored_face);
                fprintf(stderr, "Paste %d face in photo...\n", i + 1);
                paste_faces_to_input_image(restored_face, pipe_result.object[i].trans_inv, output_img);
            }
            if (pipe.ncnn) {
                ncnn::Mat gfpgan_result;
                gfpgan_NCNN_.process(pipe_result.object[i].trans_img, gfpgan_result);

                cv::Mat restored_face;
                to_ocv(gfpgan_result, restored_face);
                paste_faces_to_input_image(restored_face, pipe_result.object[i].trans_inv, output_img);
            }
        }
    }
    return 0;
}

//void onnxContext::init(const std::filesystem::path &modelPath) {
//}
//
//void onnxContext::RunModel(const std::filesystem::path &imagePath, PipelineConfig_t &pipe) {
//}
//
//void onnxContext::RunCodeformerModel(const std::filesystem::path &imagePath, PipelineConfig_t &pipe) {
//}
