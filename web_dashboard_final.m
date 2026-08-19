%% MATLAB function to create a web dashboard
% 1. Reads encrypted data from ThingSpeak
% 2. Decrypts AES-128 data
% 3. Cleans and processes data to dB
% 4. Writes the clean data back to ThingSpeak for visualization

function web_dasboard_final()
    % --- Setup ---
    channelID   = 3136461;
    writeAPIKey = 'YOUR_WRITE_API_KEY';
    readAPIKey  = 'YOUR_READ_API_KEY';

    % Must be the *exact* same key and IV as the ESP32
    aesKey = uint8([48 49 50 51 52 53 54 55 56 57 65 66 67 68 69 70]);
    aesIv  = uint8(0:15);

    % --- 1. Read Encrypted Data ---
    % Read the last 50 points from Field 1 (where ESP32 writes)
    data = thingSpeakRead(channelID, ...
        'Fields', 1, ...
        'NumPoints', 50, ...
        'OutputFormat', 'table', ...
        'ReadKey', readAPIKey);

    encryptedStrings = string(data.Field1);
    n = height(data);
    decryptedValues = NaN(n, 2); % Pre-allocate array for [adcSound, adcExtra]

    % --- 2. Decrypt Each Data Point ---
    for i = 1:n
        try
            % Decode the Base64 string back into binary ciphertext
            cipherBytes = matlab.net.base64decode(encryptedStrings(i));

            % Call the decryption helper function
            plainBytes = aesCBCDecrypt(cipherBytes, aesKey, aesIv);

            % Reconstruct the two 12-bit ADC values from the 4 bytes
            adcSound = uint16(plainBytes(1)) * 256 + uint16(plainBytes(2));
            adcExtra = uint16(plainBytes(3)) * 256 + uint16(plainBytes(4));

            decryptedValues(i, :) = double([adcSound adcExtra]);
        catch
            % If decryption fails, mark as Not-a-Number (NaN)
            decryptedValues(i, :) = [NaN NaN];
        end
    end

    % --- 3. Cleaning Data ---
    % IoT data is often messy. We must clean it.

    % Fill any gaps (e.g., failed decryptions)
    field1Clean = fillmissing(decryptedValues(:, 1), 'linear');
    field2Clean = fillmissing(decryptedValues(:, 2), 'linear');

    % Remove extreme outliers
    field1Clean = rmoutlier(field1Clean, 'median');
    field2Clean = rmoutlier(field2Clean, 'median');

    % Apply a 5-point moving median to smooth the data
    field1Clean = movmedian(field1Clean, 5);
    field2Clean = movmedian(field2Clean, 5);

    % --- 4. Normalize and Convert to dB ---
    % Convert raw 0-4095 ADC value to a normalized 0-1 range, then to dB
    % (Note: This is a relative dB scale, not absolute SPL)
    refValue = 1.0; % A reference for dB calculation

    % Normalize data to a [0, 1] range (assuming 4095 is max)
    field1Norm = field1Clean / 4095.0;
    field2Norm = field2Clean / 4095.0;

    % Convert to dB scale. Use max(val, ref) to avoid log(0).
    field1dB = 20 * log10(max(field1Norm, refValue) ./ refValue);
    field2dB = 20 * log10(max(field2Norm, refValue) ./ refValue);

    % --- 5. Upload Cleaned Data Back to ThingSpeak ---
    % Write to Fields 1 and 2 (overwriting/populating the dashboard)
    for idx = 1:n
        thingSpeakWrite(channelID, [field1dB(idx) field2dB(idx)], ...
            'Fields', [1 2], ... % Write to Field 1 and Field 2
            'WriteKey', writeAPIKey);
        pause(15); % Respect the 15-second write limit
    end
end

%% Helper function for AES-128-CBC Decryption
% NOTE: as documented in the source report, this helper is incomplete —
% it imports the Java crypto classes but the Cipher.init/doFinal calls
% that actually perform the decryption are not included. Fill in the
% standard javax.crypto.Cipher AES/CBC/NoPadding flow before use:
%   key    = SecretKeySpec(key, 'AES')
%   ivSpec = IvParameterSpec(iv)
%   cipher = Cipher.getInstance('AES/CBC/NoPadding')
%   cipher.init(Cipher.DECRYPT_MODE, key, ivSpec)
%   plainBytes = cipher.doFinal(ciphertext)
function plainBytes = aesCBCDecrypt(ciphertext, key, iv)
    % Import Java crypto libraries (MATLAB can use Java)
    import javax.crypto.Cipher
    import javax.crypto.spec.SecretKeySpec
    import javax.crypto.spec.IvParameterSpec

    % TODO: complete the Cipher setup and decryption call (see note above)
    cipher = [];
    plainBytes = [];
end
