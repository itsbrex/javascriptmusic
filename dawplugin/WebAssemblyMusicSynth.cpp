#include <JuceHeader.h>
#include <wasmedge/wasmedge.h>
#include <string> // Add this for std::string

class WebAssemblyMusicSynth; // Forward declare

class WebAssemblyMusicSynthEditor : public juce::AudioProcessorEditor,
                            private juce::ComboBox::Listener,
                            private juce::Button::Listener
{
public:
    explicit WebAssemblyMusicSynthEditor(WebAssemblyMusicSynth &p);
    void resized() override;

private:
    void comboBoxChanged(juce::ComboBox *comboBoxThatHasChanged) override;
    void buttonClicked(juce::Button* button) override;

    WebAssemblyMusicSynth &processor;
    juce::ComboBox instrumentSelector;
    juce::TextButton browseButton { "Browse Wasm File" };
    juce::Label wasmFileLabel;
    std::unique_ptr<juce::FileChooser> wasmChooser;

    // Added for Wasm download feature
    juce::TextEditor accessMessageInput;
    juce::TextButton downloadButton { "Download & Load Wasm" };
};
class WebAssemblyMusicSynth final : public AudioProcessor
{
public:
    WebAssemblyMusicSynth()
        : AudioProcessor(BusesProperties().withOutput("Output", AudioChannelSet::stereo()))
    {
    }

    void compileAndLoadWasm(const juce::String& wasmPath)
    {
        juce::File wasmFile(wasmPath);
        juce::File tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
        juce::String baseName = wasmFile.getFileName();
        juce::String tempWasmSo = tempDir.getChildFile(baseName + ".so").getFullPathName();

        printf("Compiling Wasm file: %s\n", wasmPath.toRawUTF8());
        WasmEdge_ConfigureContext *ConfCxt = WasmEdge_ConfigureCreate();
        WasmEdge_CompilerContext *CompilerCxt = WasmEdge_CompilerCreate(ConfCxt);
        WasmEdge_Result compResult = WasmEdge_CompilerCompile(CompilerCxt, wasmPath.toRawUTF8(), tempWasmSo.toRawUTF8());
        WasmEdge_CompilerDelete(CompilerCxt);
        WasmEdge_ConfigureDelete(ConfCxt);

        if (!WasmEdge_ResultOK(compResult)) {
            printf("Failed to compile Wasm file. Error code: %u\n", compResult.Code);
            return;
        }

        if (vm_cxt) {
            WasmEdge_VMDelete(vm_cxt);
            renderbuf = NULL;
        }
        vm_cxt = WasmEdge_VMCreate(NULL, NULL);
        WasmEdge_Result loadResult = WasmEdge_VMLoadWasmFromFile(vm_cxt, tempWasmSo.toRawUTF8());
        if (WasmEdge_ResultOK(loadResult)) {
            WasmEdge_VMValidate(vm_cxt);
            WasmEdge_VMInstantiate(vm_cxt);
            printf("Wasm file loaded and instantiated successfully.\n");
        } else {
            printf("Failed to load Wasm file. Error code: %u\n", loadResult.Code);
        }
    }

    static String getIdentifier()
    {
        return "WasmEdge Synth";
    }

    void prepareToPlay(double newSampleRate, int) override
    {
        synth.setCurrentPlaybackSampleRate(newSampleRate);
        printf("Samplerate is %f\n", newSampleRate);
        prepareWasm();
    }

    void prepareWasm() {
        if (vm_cxt == NULL) {
            printf("Wasm VM context is NULL\n");
            return;
        }
        double newSampleRate = synth.getSampleRate();
        if (environmentModuleInstanceContext != NULL) {
            WasmEdge_ModuleInstanceDelete(environmentModuleInstanceContext);
        }
        environmentModuleInstanceContext = WasmEdge_ModuleInstanceCreate(WasmEdge_StringCreateByCString("environment"));
        
        WasmEdge_GlobalTypeContext *SAMPLERATE_type = WasmEdge_GlobalTypeCreate(WasmEdge_ValTypeGenF32(), WasmEdge_Mutability_Const);
        WasmEdge_GlobalInstanceContext *SAMPLERATE_global = WasmEdge_GlobalInstanceCreate(SAMPLERATE_type, WasmEdge_ValueGenF32(newSampleRate));
        WasmEdge_ModuleInstanceAddGlobal(environmentModuleInstanceContext, WasmEdge_StringCreateByCString("SAMPLERATE"), SAMPLERATE_global);
        WasmEdge_VMRegisterModuleFromImport(vm_cxt, environmentModuleInstanceContext);

        WasmEdge_VMValidate(vm_cxt);
        printf("Wasm module validatedf\n");
        WasmEdge_VMInstantiate(vm_cxt);

        printf("Wasm module instantiated\n");

        const WasmEdge_ModuleInstanceContext *moduleCtx = WasmEdge_VMGetActiveModule(vm_cxt);
        WasmEdge_GlobalInstanceContext *globCtx = WasmEdge_ModuleInstanceFindGlobal(moduleCtx, WasmEdge_StringCreateByCString("samplebuffer"));
        WasmEdge_MemoryInstanceContext *memCtx = WasmEdge_ModuleInstanceFindMemory(moduleCtx, WasmEdge_StringCreateByCString("memory"));

        fillSampleBufferFuncNameString = WasmEdge_StringCreateByCString("fillSampleBufferWithNumSamples");
        WasmEdge_Value globValue = WasmEdge_GlobalInstanceGetValue(globCtx);
        uint32_t sampleBufferAddrValue = WasmEdge_ValueGetI32(globValue);

        const uint8_t *renderbytebuf = WasmEdge_MemoryInstanceGetPointer(memCtx, sampleBufferAddrValue, 128 * 2 * 4);
        renderbuf = (float32_t *)renderbytebuf;
        printf("Wasm module exports stored\n");

        printf("Prepare completed\n");
    }

    void selectInstrument(int instrumentId)
    {
        selectedInstrumentId = instrumentId;
        printf("Selected instrument ID: %d\n", instrumentId);
    }

    void loadWasmFile(const juce::String& filePath)
    {
        compileAndLoadWasm(filePath);
        prepareWasm();
    }

    void releaseResources() override
    {
    }

    void processBlock(AudioBuffer<float> &buffer, MidiBuffer &midiMessages) override
    {
        if (renderbuf == NULL)
        {
            return;
        }
        for (const auto metadata : midiMessages)
        {
            MidiMessage message = metadata.getMessage();
            const uint8 *rawmessage = message.getRawData();

            // Copy the message so we can modify the channel
            uint8_t msg0 = (rawmessage[0] & 0xF0) | ((selectedInstrumentId - 1) & 0x0F);

            WasmEdge_Value args[3];
            args[0] = WasmEdge_ValueGenI32(msg0);
            args[1] = WasmEdge_ValueGenI32((uint8_t)rawmessage[1]);
            args[2] = WasmEdge_ValueGenI32((uint8_t)rawmessage[2]);
            WasmEdge_VMExecute(vm_cxt, WasmEdge_StringCreateByCString("shortmessage"), args, 3, NULL, 0);

            printf("sent midi to wasm synth: %d, %d, %d (channel %d)\n", msg0, rawmessage[1], rawmessage[2], (selectedInstrumentId - 1));
        }

        int numSamples = buffer.getNumSamples();
        auto *left = buffer.getWritePointer(0);
        auto *right = buffer.getWritePointer(1);

        for (int sampleNo = 0; sampleNo < numSamples; sampleNo += 128)
        {
            int numSamplesToRender = std::min(numSamples - sampleNo, 128);

            WasmEdge_Value args[1] = {WasmEdge_ValueGenI32((uint32_t)numSamplesToRender)};
            WasmEdge_Result result = WasmEdge_VMExecute(vm_cxt, fillSampleBufferFuncNameString, args, 1, NULL, 0);

            for (int ndx = 0; ndx < numSamplesToRender; ndx++)
            {
                left[sampleNo + ndx] = renderbuf[ndx] * 0.3;
                right[sampleNo + ndx] = renderbuf[ndx + 128] * 0.3;
            }
        }
    }

    using AudioProcessor::processBlock;

    const String getName() const override { return getIdentifier(); }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    AudioProcessorEditor *createEditor() override
    {
        return new WebAssemblyMusicSynthEditor(*this);
    }

    bool hasEditor() const override
    {
        return true;
    }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const String getProgramName(int) override { return {}; }
    void changeProgramName(int, const String &) override {}
    void getStateInformation(juce::MemoryBlock &) override {}
    void setStateInformation(const void *, int) override {}

private:
    int selectedInstrumentId = 1; // Default to 1 (Piano)
    WasmEdge_VMContext *vm_cxt;
    WasmEdge_ModuleInstanceContext *environmentModuleInstanceContext;
    WasmEdge_String fillSampleBufferFuncNameString;
    float32_t *renderbuf = NULL;
    Synthesiser synth;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebAssemblyMusicSynth)
};

WebAssemblyMusicSynthEditor::WebAssemblyMusicSynthEditor(WebAssemblyMusicSynth &p)
    : juce::AudioProcessorEditor(p), processor(p)
{
    setSize(400, 200);
    instrumentSelector.addItem("Channel 1", 1);
    instrumentSelector.addItem("Channel 2", 2);
    instrumentSelector.addItem("Channel 3", 3);
    instrumentSelector.addItem("Channel 4", 4);
    instrumentSelector.addItem("Channel 5", 5);
    instrumentSelector.addItem("Channel 6", 6);
    instrumentSelector.addItem("Channel 7", 7);
    instrumentSelector.addItem("Channel 8", 8);
    instrumentSelector.addItem("Channel 9", 9);
    instrumentSelector.addItem("Channel 10", 10);
    instrumentSelector.addItem("Channel 11", 11);
    instrumentSelector.addItem("Channel 12", 12);
    instrumentSelector.addItem("Channel 13", 13);
    instrumentSelector.addItem("Channel 14", 14);
    instrumentSelector.addItem("Channel 15", 15);
    instrumentSelector.addItem("Channel 16", 16);
    instrumentSelector.setSelectedId(1);
    instrumentSelector.addListener(this);
    addAndMakeVisible(instrumentSelector);

    addAndMakeVisible(browseButton);
    browseButton.addListener(this);
    addAndMakeVisible(wasmFileLabel);
    wasmFileLabel.setText("No wasm file selected", juce::dontSendNotification);

    // Initialize and add new UI elements for Wasm download
    addAndMakeVisible(accessMessageInput);
    accessMessageInput.setMultiLine(false);
    accessMessageInput.setReturnKeyStartsNewLine(false);
    accessMessageInput.setScrollbarsShown(false);
    accessMessageInput.setTextToShowWhenEmpty("Enter Access Message here", juce::Colours::grey);

    addAndMakeVisible(downloadButton);
    downloadButton.addListener(this);
}

void WebAssemblyMusicSynthEditor::resized()
{
    instrumentSelector.setBounds(10, 10, getWidth() - 20, 30);
    browseButton.setBounds(10, 50, getWidth() - 20, 30);
    wasmFileLabel.setBounds(10, 90, getWidth() - 20, 24);

    // Position new UI elements
    accessMessageInput.setBounds(10, 120, getWidth() - 20, 24);
    downloadButton.setBounds(10, 150, getWidth() - 20, 30);
}

void WebAssemblyMusicSynthEditor::comboBoxChanged(juce::ComboBox *comboBoxThatHasChanged)
{
    if (comboBoxThatHasChanged == &instrumentSelector)
        processor.selectInstrument(instrumentSelector.getSelectedId());
}

void WebAssemblyMusicSynthEditor::buttonClicked(juce::Button* button)
{
    if (button == &browseButton)
    {
        wasmChooser = std::make_unique<juce::FileChooser>(
            "Select a Wasm file",
            juce::File(),
            "*.wasm"
        );

        auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

        wasmChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& fc)
        {
            auto selectedFile = fc.getResult();
            if (selectedFile.existsAsFile())
            {
                wasmFileLabel.setText(selectedFile.getFullPathName(), juce::dontSendNotification);
                processor.loadWasmFile(selectedFile.getFullPathName());
            }
        });
    }
    else if (button == &downloadButton)
    {
        juce::String accessMessage = accessMessageInput.getText();
        if (accessMessage.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Input Error",
                                                   "Please enter an access message.");
            return;
        }

        juce::StringArray accessMessageParts;
        accessMessageParts.addTokens(accessMessage, ".", "");
        if (accessMessageParts.size() != 2)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Input Error",
                                                   "Invalid access message format.");
            return;
        }

        juce::String messageJsonBase64 = accessMessageParts[0];
        juce::String signature = accessMessageParts[1];
        
        juce::MemoryBlock messageDecodedBytes;
        juce::MemoryOutputStream messageOutputStream(messageDecodedBytes, false);
        if (!juce::Base64::convertFromBase64(messageOutputStream, messageJsonBase64)) {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Input Error", "Failed to decode access message (first part).");
            return;
        }
        // Ensure the stream is flushed and the MemoryBlock is updated if necessary, though for MemoryOutputStream it's usually direct.
        messageOutputStream.flush(); // Good practice
        juce::String messageJsonString = messageDecodedBytes.toString();

        juce::var messageVar = juce::JSON::parse(messageJsonString);
        if (messageVar == juce::var()) // Check if parsing failed
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Input Error",
                                                   "Failed to parse access message JSON.");
            return;
        }

        juce::String tokenId = messageVar["token_id"].toString();
        if (tokenId.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Input Error",
                                                   "Could not find 'token_id' in access message.");
            return;
        }

        juce::DynamicObject::Ptr args = new juce::DynamicObject();
        args->setProperty("function_name", "get_locked_content");
        args->setProperty("message", messageJsonString); // Use the original JSON string
        args->setProperty("signature", signature);
        args->setProperty("token_id", tokenId);

        juce::String argsJsonString = juce::JSON::toString(juce::var(args));
        juce::String argsBase64 = juce::Base64::toBase64(argsJsonString);

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("method", "query");
        
        juce::DynamicObject::Ptr params = new juce::DynamicObject();
        params->setProperty("request_type", "call_function");
        params->setProperty("finality", "optimistic");
        params->setProperty("account_id", "webassemblymusic.near");
        params->setProperty("method_name", "call_js_func");
        params->setProperty("args_base64", argsBase64);
        
        payload->setProperty("params", juce::var(params));
        payload->setProperty("id", 132); // Static ID as in example
        payload->setProperty("jsonrpc", "2.0");

        juce::String payloadJsonString = juce::JSON::toString(juce::var(payload));

        juce::URL baseUrl("https://rpc.mainnet.fastnear.com/");
        
        // Set the raw JSON string as the body of the request
        juce::URL urlToUse = baseUrl.withPOSTData(payloadJsonString); 
        
        int statusCode = 0;
        juce::StringPairArray responseHeaders;

        // Construct InputStreamOptions and chain settings.
        // Each 'withX' method returns a new InputStreamOptions object by value, so the whole chain must be used in initialization.
        juce::URL::InputStreamOptions options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)                                                   
                                                 .withExtraHeaders("Content-Type: application/json\n")
                                                 .withHttpRequestCmd("POST")
                                                 .withConnectionTimeoutMs(15000)
                                                 .withResponseHeaders(&responseHeaders)
                                                 .withStatusCode(&statusCode);
        
        std::unique_ptr<juce::InputStream> stream = urlToUse.createInputStream(options);

        if (stream == nullptr) { // Check if stream creation failed
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, 
                                                   "Network Error", 
                                                   "Failed to create input stream. Status code: " + juce::String(statusCode) + ". Check network connection and URL.");
            return;
        }

        if (statusCode == 200)
        {
            juce::MemoryBlock wasmBytes;

            juce::Logger::writeToLog("Successfully received response from NEAR RPC.");
            juce::String responseBody = stream->readEntireStreamAsString();
            juce::var responseVar;
            juce::Result parseResult = juce::JSON::parse(responseBody, responseVar);

            if (parseResult.failed())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Parse Error", "Failed to parse JSON response from server.");
                return;
            }

            juce::var rpcResult = responseVar["result"];
            if (!rpcResult.isObject()) {
                 juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Invalid 'result' object in server response.");
                return;
            }

            juce::var charCodeArrayVar = rpcResult["result"]; 
            if (!charCodeArrayVar.isArray())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Wasm data (inside result.result) not found or not an array in server response.");
                return;
            }

            // Step 1: Create a MemoryBlock of bytes from charCodeArrayVar
            juce::MemoryBlock charCodeBytes;
            if (juce::Array<juce::var>* charCodeArray = charCodeArrayVar.getArray())
            {
                for (const auto& charCodeVar : *charCodeArray)
                {
                    if (charCodeVar.isInt() || charCodeVar.isDouble()) // isInt() should be sufficient for byte values
                    {
                        int val = static_cast<int>(charCodeVar);
                        if (val >= 0 && val <= 255)
                        {
                            uint8_t byteVal = static_cast<uint8_t>(val);
                            charCodeBytes.append(&byteVal, 1);
                        }
                        else
                        {
                            juce::Logger::writeToLog("Error: Character code value out of 0-255 range: " + juce::String(val));
                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Invalid character code (out of 0-255 range) in Wasm data array.");
                            return;
                        }
                    }
                    else
                    {
                        juce::Logger::writeToLog("Error: Non-numeric value in character code array.");
                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Invalid (non-numeric) character code in Wasm data array.");
                        return;
                    }
                }
            }
            else // Should not be reached if the isArray check above is comprehensive
            {
                 juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Failed to get character code array from Wasm data (internal error).");
                return;
            }

            if (charCodeBytes.isEmpty() && charCodeArrayVar.getArray() != nullptr && charCodeArrayVar.getArray()->size() > 0) {
                juce::Logger::writeToLog("Error: Constructed byte array for JSON string is empty but char code array was not.");
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Reconstructed Wasm string is empty but char code array was not. Possible non-printable chars or other issue.");
                return;
            }

            // Step 2: Convert this block of bytes to a string (UTF-8 assumed by JUCE's toString)
            juce::String jsonStringifiedBase64 = charCodeBytes.toString();
            juce::Logger::writeToLog("String from char codes (expected JSON-stringified Base64, sample): " + (jsonStringifiedBase64.length() > 200 ? jsonStringifiedBase64.substring(0,100) + "..." + jsonStringifiedBase64.substring(jsonStringifiedBase64.length()-100) : jsonStringifiedBase64));
            juce::Logger::writeToLog("Length of jsonStringifiedBase64: " + juce::String(jsonStringifiedBase64.length()));

            // Step 3: Manually extract content from the JSON-like string
            juce::String actualContentString;
            if (jsonStringifiedBase64.length() >= 2 && jsonStringifiedBase64.startsWithChar('"') && jsonStringifiedBase64.endsWithChar('"'))
            {
                actualContentString = jsonStringifiedBase64.substring(1, jsonStringifiedBase64.length() - 1);
                juce::Logger::writeToLog("Successfully stripped outer quotes. Extracted content length: " + juce::String(actualContentString.length()));
            }
            else
            {
                juce::Logger::writeToLog("Error: String from char codes is not properly quoted or is too short. Length: " + juce::String(jsonStringifiedBase64.length()) + ". Content (first 200 chars): " + jsonStringifiedBase64.substring(0, juce::jmin(200, jsonStringifiedBase64.length())));
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Wasm data string from server is not in the expected JSON-quoted format. Check logs.");
                return;
            }
            // actualContentString now holds the (hopefully) raw Base64 data.

            // Step 4: Sanitize actualContentString to get pureBase64Data -- REMOVED
            // The string used for decoding is now actualContentString directly.

            juce::Logger::writeToLog("Using actualContentString directly for Base64 decoding (sample): " + (actualContentString.length() > 200 ? actualContentString.substring(0,100) + "..." + actualContentString.substring(actualContentString.length()-100) : actualContentString));
            juce::Logger::writeToLog("Full actualContentString length (juce::String::length()): " + juce::String(actualContentString.length()));
            juce::Logger::writeToLog("Full actualContentString numBytesAsUTF8 (incl. null): " + juce::String(actualContentString.getNumBytesAsUTF8()));

            // Log first and last few character codes of actualContentString
            if (actualContentString.isNotEmpty()) {
                juce::String firstCharsLog = "actualContentString first 10 char codes: ";
                for (int i = 0; i < juce::jmin(10, actualContentString.length()); ++i) {
                    firstCharsLog += juce::String((int)actualContentString[i]) + " ";
                }
                juce::Logger::writeToLog(firstCharsLog);

                if (actualContentString.length() > 10) {
                    juce::String lastCharsLog = "actualContentString last 10 char codes: ";
                    for (int i = juce::jmax(0, actualContentString.length() - 10); i < actualContentString.length(); ++i) {
                        lastCharsLog += juce::String((int)actualContentString[i]) + " ";
                    }
                    juce::Logger::writeToLog(lastCharsLog);
                }
            }

            // Step 5: Decode the Base64 string (now using actualContentString)
            // wasmBytes is declared at the beginning of the if (statusCode == 200) block
            wasmBytes.reset(); // Clear for new data
            juce::MemoryOutputStream outputStream(wasmBytes, false);

            if (!actualContentString.isEmpty()) { // Check actualContentString
                if (!juce::Base64::convertFromBase64(outputStream, actualContentString)) // Use actualContentString
                {
                    juce::Logger::writeToLog("Base64 decoding failed for actualContentString (using MemoryOutputStream).");
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Failed to decode Base64 Wasm data (using actualContentString with MemoryOutputStream).");
                    return;
                }
            }
            outputStream.flush(); // Ensure all data is written to wasmBytes
            juce::Logger::writeToLog("Size of wasmBytes after Base64 decoding (using MemoryOutputStream): " + juce::String(wasmBytes.getSize()));

            if (wasmBytes.isEmpty() && !actualContentString.isEmpty()) // Check actualContentString
            {
                juce::Logger::writeToLog("Error: wasmBytes is empty after Base64 decoding, but input (actualContentString) was not. Possible issues with the source data or decoding process.");
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Download Error", "Decoded Wasm data is empty despite non-empty input (actualContentString). Server might have sent an empty or invalid module, or decoding failed silently.");
                return;
            }

            // Step 6: Save the decoded Wasm data to a temporary file using juce::TemporaryFile
            juce::TemporaryFile tempFileHolder (juce::String("downloaded_synth.wasm")); 
            
            juce::File actualTempFile = tempFileHolder.getFile();

            if (actualTempFile == juce::File{}) // Check if the file object is invalid
            {
                juce::Logger::writeToLog("Error: Failed to create temporary file object using juce::TemporaryFile.");
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "File Error", "Failed to initialize temporary file structure.");
                return;
            }
            
            juce::Logger::writeToLog("Attempting to save Wasm to temporary file: " + actualTempFile.getFullPathName());

            if (actualTempFile.replaceWithData(wasmBytes.getData(), wasmBytes.getSize()))
            {
                juce::Logger::writeToLog("Successfully saved Wasm to: " + actualTempFile.getFullPathName() + " Size: " + juce::String(actualTempFile.getSize()));

                processor.compileAndLoadWasm(actualTempFile.getFullPathName());
                processor.prepareWasm(); 
            }
            else
            {
                juce::Logger::writeToLog("Error: Failed to write Wasm data to temporary file: " + actualTempFile.getFullPathName());
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "File Error", "Failed to save downloaded Wasm file using TemporaryFile.");
            }
        }
        else
        {
            juce::String errorBody;
            if(stream) errorBody = stream->readEntireStreamAsString(); // Try to read error body if stream exists
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Download Error",
                                                   "HTTP Error: " + juce::String(statusCode) + "\\nResponse: " + errorBody);
        }
        // No explicit 'else' for stream == nullptr needed here as it's handled above.
        // The 'else' above corresponds to 'if (statusCode == 200)'
    }
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new WebAssemblyMusicSynth();
}